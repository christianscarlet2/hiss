"""
sm_tools.py — boto3-backed SageMaker primitives for the Autonomous Improvement Loop.

The reusable core the AIL drives (via cron/loop) and that a thin MCP wrapper exposes as
sm_* tools. DORMANT until AWS creds exist on the box (aws configure --profile hiss):
every call lazy-imports boto3 and returns a clear "no creds" message otherwise, so this
imports safely today.

Loop: export_training_data -> start_training -> wait -> evaluate vs champion -> if better
& passes adversarial replay -> register + promote (deploy TorchScript to daemons) -> journal.
"""
from __future__ import annotations
import os, time

PROFILE = os.environ.get("HISS_AWS_PROFILE", "hiss")
REGION = os.environ.get("AWS_DEFAULT_REGION", "us-east-1")
BUCKET = os.environ.get("HISS_S3_BUCKET", "scarletbeast-hiss")
ROLE_ENV = "HISS_SM_ROLE_ARN"   # SageMaker execution role ARN (created once, below)


def _sess():
    """Return (boto3_session, error_str). error_str set if boto3/creds unavailable."""
    try:
        import boto3
        from botocore.exceptions import BotoCoreError, ClientError  # noqa: F401
    except Exception as e:
        return None, f"boto3 not installed ({e}). Run: pip install boto3"
    try:
        s = boto3.Session(profile_name=PROFILE, region_name=REGION)
        s.client("sts").get_caller_identity()
        return s, None
    except Exception:
        try:
            s = boto3.Session(region_name=REGION)   # fall back to default/env creds
            s.client("sts").get_caller_identity()
            return s, None
        except Exception as e:
            return None, (f"No working AWS creds for profile '{PROFILE}' or default. "
                          f"Run: aws configure --profile {PROFILE}  ({e})")


def status() -> dict:
    s, err = _sess()
    if err:
        return {"ok": False, "error": err}
    ident = s.client("sts").get_caller_identity()
    return {"ok": True, "account": ident["Account"], "arn": ident["Arn"], "region": REGION,
            "bucket": BUCKET, "role": os.environ.get(ROLE_ENV, "(unset — run ensure_execution_role)")}


def ensure_bucket() -> dict:
    s, err = _sess()
    if err:
        return {"ok": False, "error": err}
    s3 = s.client("s3")
    try:
        s3.head_bucket(Bucket=BUCKET)
        return {"ok": True, "bucket": BUCKET, "created": False}
    except Exception:
        kw = {} if REGION == "us-east-1" else {"CreateBucketConfiguration": {"LocationConstraint": REGION}}
        s3.create_bucket(Bucket=BUCKET, **kw)
        return {"ok": True, "bucket": BUCKET, "created": True}


def ensure_execution_role(role_name="HissSageMakerRole") -> dict:
    """Create the role SageMaker assumes (S3 + SageMaker). Idempotent. Prints the ARN to set in HISS_SM_ROLE_ARN."""
    s, err = _sess()
    if err:
        return {"ok": False, "error": err}
    import json
    iam = s.client("iam")
    trust = json.dumps({"Version": "2012-10-17", "Statement": [
        {"Effect": "Allow", "Principal": {"Service": "sagemaker.amazonaws.com"}, "Action": "sts:AssumeRole"}]})
    try:
        iam.create_role(RoleName=role_name, AssumeRolePolicyDocument=trust,
                        Description="Hiss SageMaker execution role")
    except iam.exceptions.EntityAlreadyExistsException:
        pass
    for arn in ("arn:aws:iam::aws:policy/AmazonSageMakerFullAccess",
                "arn:aws:iam::aws:policy/AmazonS3FullAccess"):
        try:
            iam.attach_role_policy(RoleName=role_name, PolicyArn=arn)
        except Exception:
            pass
    arn = iam.get_role(RoleName=role_name)["Role"]["Arn"]
    return {"ok": True, "role_arn": arn, "note": f"export {ROLE_ENV}={arn}"}


def start_training(entry_point="train.py", source_dir=None, instance_type="ml.m5.large",
                   spot=True, hyperparameters=None) -> dict:
    """Launch a PyTorch script-mode training job on Spot. Data: s3://BUCKET/features/."""
    s, err = _sess()
    if err:
        return {"ok": False, "error": err}
    role = os.environ.get(ROLE_ENV)
    if not role:
        return {"ok": False, "error": f"{ROLE_ENV} unset — run ensure_execution_role first"}
    from sagemaker.pytorch import PyTorch          # lazy: needs `pip install sagemaker`
    from sagemaker.session import Session
    sess = Session(boto_session=s)
    job = f"hiss-train-{int(time.time())}"
    est = PyTorch(
        entry_point=entry_point,
        source_dir=source_dir or os.path.dirname(os.path.abspath(__file__)),
        role=role, instance_count=1, instance_type=instance_type,
        framework_version="2.3", py_version="py311",
        hyperparameters=hyperparameters or {"epochs": 8, "width": 384},
        use_spot_instances=spot, max_run=3600, max_wait=7200 if spot else None,
        sagemaker_session=sess, base_job_name="hiss-train")
    est.fit({"train": f"s3://{BUCKET}/features/"}, job_name=job, wait=False)
    return {"ok": True, "job": job, "instance": instance_type, "spot": spot}


def describe_training(job: str) -> dict:
    s, err = _sess()
    if err:
        return {"ok": False, "error": err}
    d = s.client("sagemaker").describe_training_job(TrainingJobName=job)
    return {"ok": True, "job": job, "status": d["TrainingJobStatus"],
            "secondary": d.get("SecondaryStatus"),
            "model_artifact": d.get("ModelArtifacts", {}).get("S3ModelArtifacts"),
            "billable_seconds": d.get("BillableTimeInSeconds")}


# evaluate(model)/promote(model)/rollback are thin wrappers over the replay regression
# suite + the in-process model swap on the daemons — implemented alongside the daemon
# deploy mechanism (see SAGEMAKER_TRAINING_PLAN.md §5-7). Left as named stubs here so the
# AIL/MCP surface is complete and discoverable:
def evaluate(model_artifact: str) -> dict:
    return {"ok": False, "todo": "run replay EV/100 + adversarial suite vs champion (plan §5)",
            "model_artifact": model_artifact}


def promote(model_artifact: str) -> dict:
    return {"ok": False, "todo": "push TorchScript to daemons + flip openai_action override (plan §6)",
            "model_artifact": model_artifact}
