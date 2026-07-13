import psycopg2, os
dsn = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
c = psycopg2.connect(dsn); cur = c.cursor()
cur.execute("""SELECT player,gametype,vpip_n,vpip_d,pfr_n,pfr_d,aggr_actions,call_actions,wtsd_n,wtsd_d,
               f3b_n,f3b_d,ftc_n,ftc_d FROM hud_player_stats WHERE vpip_d>=25 ORDER BY vpip_d DESC LIMIT 12""")
print("--- villains (name, hands, VPIP, PFR, AF, WTSD, Fold3bet, FoldCbet) ---")
nits=tags=lags=stations=0
for r in cur.fetchall():
    name,gt,vn,vd,pn,pd,ag,ca,wn,wd,f3n,f3d,ftn,ftd = r
    vpip=100.0*vn/vd if vd else 0; pfr=100.0*pn/pd if pd else 0
    af=(ag/ca) if ca else 0; wtsd=100.0*wn/wd if wd else 0
    f3b=100.0*f3n/f3d if f3d else 0; fcb=100.0*ftn/ftd if ftd else 0
    print("  %-16s h=%-4d VPIP=%4.1f PFR=%4.1f AF=%4.2f WTSD=%4.1f F3B=%4.1f FCb=%4.1f"%(name[:16],vd,vpip,pfr,af,wtsd,f3b,fcb))
# distribution of archetypes across all known players
cur.execute("""SELECT vpip_n,vpip_d,pfr_n,pfr_d,aggr_actions,call_actions FROM hud_player_stats WHERE vpip_d>=20""")
for vn,vd,pn,pd,ag,ca in cur.fetchall():
    vpip=100.0*vn/vd if vd else 0; af=(ag/ca) if ca else 0
    if vpip<15: nits+=1
    elif 15<=vpip<=26 and af>=1.5: tags+=1
    elif vpip>26 and af>2.0: lags+=1
    elif vpip>30 and af<1.2: stations+=1
print("KNOWN-PLAYER ARCHETYPE MIX (vpip_d>=20): nits=%d tags=%d lags=%d stations=%d"%(nits,tags,lags,stations))
