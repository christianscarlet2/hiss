"""
train.py — SageMaker PyTorch *script mode* trainer for the Hiss poker policy.

A compact feature-tower MLP with three heads:
  - policy  : action class (fold/check/call/raise/allin)            [cross-entropy]
  - betsize : raise size as a fraction of pot, in [0,1]             [MSE, masked to raises]
  - value   : hand EV (chips won/lost, normalized in bb)            [MSE]

Phase-1 = supervised imitation + value on logged hands (the OHF bot's decisions +
replay outcomes). Cheap, stable, serves as an advisor that A/Bs against the OHF.
Exports BOTH a state_dict (for resume) and a TorchScript module (for in-process
inference inside the Linux Hiss daemon — no standing endpoint bill).

Run locally:  python train.py --train ./data --epochs 5 --model-dir ./model
On SageMaker: PyTorch estimator, entry_point=train.py; channels map to SM_CHANNEL_*.
Reads .parquet or .jsonl rows: {"sym":{...}, "hole","board","action","raiseto","reward_bb"}.
"""
from __future__ import annotations
import argparse, glob, json, os
import numpy as np

import features as F  # same dir, the shared infoset contract


def _load_rows(path):
    rows = []
    for fp in sorted(glob.glob(os.path.join(path, "**", "*"), recursive=True)):
        if fp.endswith(".jsonl"):
            with open(fp) as fh:
                rows += [json.loads(l) for l in fh if l.strip()]
        elif fp.endswith(".parquet"):
            import pandas as pd  # only needed for parquet
            df = pd.read_parquet(fp)
            rows += df.to_dict("records")
    return rows


def _build_xy(rows):
    X = np.zeros((len(rows), F.N_FEATURES), dtype=np.float32)
    y_act = np.zeros(len(rows), dtype=np.int64)
    y_bet = np.zeros(len(rows), dtype=np.float32)
    y_val = np.zeros(len(rows), dtype=np.float32)
    for i, r in enumerate(rows):
        # parquet flattens sym/* into columns; re-nest if needed
        if "sym" not in r:
            r = {"sym": {k: r[k] for k in r if k not in ("hole", "board", "action", "raiseto", "reward_bb")},
                 **{k: r.get(k) for k in ("hole", "board", "action", "raiseto", "reward_bb")}}
        X[i] = F.vectorize(r)
        y_act[i] = F.action_index(r.get("action"))
        y_bet[i] = F.betsize_fraction(r)
        y_val[i] = float(r.get("reward_bb", 0.0) or 0.0)
    return X, y_act, y_bet, y_val


def main():
    import torch
    import torch.nn as nn

    ap = argparse.ArgumentParser()
    ap.add_argument("--train", default=os.environ.get("SM_CHANNEL_TRAIN", "./data"))
    ap.add_argument("--model-dir", default=os.environ.get("SM_MODEL_DIR", "./model"))
    ap.add_argument("--epochs", type=int, default=int(os.environ.get("EPOCHS", 8)))
    ap.add_argument("--batch", type=int, default=int(os.environ.get("BATCH", 1024)))
    ap.add_argument("--lr", type=float, default=float(os.environ.get("LR", 1e-3)))
    ap.add_argument("--width", type=int, default=int(os.environ.get("WIDTH", 384)))
    args = ap.parse_args()

    rows = _load_rows(args.train)
    if not rows:
        raise SystemExit(f"no training rows under {args.train}")
    X, y_act, y_bet, y_val = _build_xy(rows)

    # standardize features (save the stats for inference)
    mu, sd = X.mean(0), X.std(0) + 1e-6
    Xn = (X - mu) / sd

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    Xt = torch.tensor(Xn, device=dev)
    at = torch.tensor(y_act, device=dev)
    bt = torch.tensor(y_bet, device=dev)
    vt = torch.tensor(y_val, device=dev)
    raise_mask = (at >= F.ACTION_TO_IDX["raise"]).float()

    class Net(nn.Module):
        def __init__(self, n_in, w, n_act):
            super().__init__()
            self.tower = nn.Sequential(
                nn.Linear(n_in, w), nn.ReLU(), nn.LayerNorm(w),
                nn.Linear(w, w), nn.ReLU(), nn.LayerNorm(w),
                nn.Linear(w, w // 2), nn.ReLU())
            self.policy = nn.Linear(w // 2, n_act)
            self.betsize = nn.Sequential(nn.Linear(w // 2, 1), nn.Sigmoid())
            self.value = nn.Linear(w // 2, 1)

        def forward(self, x):
            h = self.tower(x)
            return self.policy(h), self.betsize(h).squeeze(-1), self.value(h).squeeze(-1)

    net = Net(F.N_FEATURES, args.width, len(F.ACTION_CLASSES)).to(dev)
    opt = torch.optim.Adam(net.parameters(), lr=args.lr)
    ce, mse = nn.CrossEntropyLoss(), nn.MSELoss(reduction="none")
    n = len(Xt)

    for ep in range(args.epochs):
        net.train(); perm = torch.randperm(n, device=dev); tot = 0.0
        for s in range(0, n, args.batch):
            idx = perm[s:s + args.batch]
            logits, bet, val = net(Xt[idx])
            loss_p = ce(logits, at[idx])
            m = raise_mask[idx]
            loss_b = (mse(bet, bt[idx]) * m).sum() / (m.sum() + 1e-6)
            loss_v = mse(val, vt[idx]).mean()
            loss = loss_p + 0.5 * loss_b + 0.25 * loss_v
            opt.zero_grad(); loss.backward(); opt.step()
            tot += loss.item() * len(idx)
        with torch.no_grad():
            acc = (net(Xt)[0].argmax(1) == at).float().mean().item()
        print(f"epoch {ep+1}/{args.epochs} loss={tot/n:.4f} action_acc={acc:.3f}", flush=True)

    os.makedirs(args.model_dir, exist_ok=True)
    torch.save({"state_dict": net.state_dict(), "mu": mu, "sd": sd,
                "feature_columns": F.FEATURE_COLUMNS, "action_classes": F.ACTION_CLASSES,
                "width": args.width}, os.path.join(args.model_dir, "model.pt"))
    # TorchScript for in-process inference in the Linux Hiss daemon (CPU, no endpoint)
    net.eval()
    ts = torch.jit.trace(net.cpu(), torch.zeros(1, F.N_FEATURES))
    ts.save(os.path.join(args.model_dir, "model_scripted.pt"))
    np.savez(os.path.join(args.model_dir, "norm.npz"), mu=mu, sd=sd)
    print(f"saved model + TorchScript to {args.model_dir}", flush=True)


if __name__ == "__main__":
    main()
