#!/usr/bin/env python3
from pathlib import Path
import pandas as pd
import matplotlib.pyplot as plt
import sys

ROOT = Path(__file__).resolve().parent.parent
HASH_DIR = ROOT / "logs" / "hash"
CSV_DIR = HASH_DIR / "csv"
CSV_DIR.mkdir(parents=True, exist_ok=True)
PNG_DIR = HASH_DIR / "png"
PNG_DIR.mkdir(parents=True, exist_ok=True)

SHOW = "--show" in sys.argv
args = [a for a in sys.argv[1:] if a != "--show"]

if len(args) >= 1:
    csv_path = Path(args[0]).resolve()
else:
    csv_files = sorted(CSV_DIR.glob("*.csv"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not csv_files:
        print(f"Erreur : aucun CSV trouvé dans '{CSV_DIR}'.")
        sys.exit(1)
    csv_path = csv_files[0]

if not csv_path.exists():
    print(f"Erreur : Le fichier '{csv_path}' est introuvable.")
    sys.exit(1)

try:
    df = pd.read_csv(csv_path, sep=";")
except Exception as e:
    print(f"Erreur lors de la lecture du CSV : {e}")
    sys.exit(1)

required_columns = [
    "MapId", "RootKey", "Player", "Occupied", "Depth", "ElapsedMs",
    "Nodes", "Leafs", "Terminals", "HashCalls", "NPS", "Probes",
    "Hits", "HitRate", "Cutoffs", "CutoffRate", "MoveHits",
    "LegalTTMoves", "OrderRate", "ExactHits", "NewEntries",
    "Overwrites", "RejectedCollisions", "FillRate"
]

missing = [c for c in required_columns if c not in df.columns]
if missing:
    print("Erreur : colonnes manquantes dans le CSV :", ", ".join(missing))
    sys.exit(1)

numeric_columns = [
    "Player", "Occupied", "Depth", "ElapsedMs", "Nodes", "Leafs",
    "Terminals", "HashCalls", "NPS", "Probes", "Hits", "HitRate",
    "Cutoffs", "CutoffRate", "MoveHits", "LegalTTMoves", "OrderRate",
    "ExactHits", "NewEntries", "Overwrites", "RejectedCollisions", "FillRate"
]

for col in numeric_columns:
    df[col] = pd.to_numeric(df[col], errors="coerce")

df = df.dropna(subset=["Depth"]).copy()
df["Depth"] = df["Depth"].astype(int)
df["NodesPerMs"] = df["Nodes"] / df["ElapsedMs"].replace(0, pd.NA)
df["MsPerMNodes"] = (df["ElapsedMs"] * 1_000_000.0) / df["Nodes"].replace(0, pd.NA)

if df.empty:
    print("Erreur : aucune donnée exploitable après parsing.")
    sys.exit(1)

map_id = str(df["MapId"].iloc[0])
root_count = df["RootKey"].nunique()

# Agrégation par profondeur pour obtenir une vue "par map"
summary = (
    df.groupby("Depth", as_index=False)
      .agg({
          "NPS": "median",
          "HitRate": "mean",
          "CutoffRate": "mean",
          "OrderRate": "mean",
          "Nodes": "median",
          "NewEntries": "median",
          "Overwrites": "median",
          "FillRate": "mean",
      })
      .sort_values("Depth")
)

stem = csv_path.stem

def extract_family_name(stem: str, map_id: str) -> str:
    suffix = f"_{map_id}"
    if stem.endswith(suffix):
        return stem[:-len(suffix)]
    parts = stem.split("_")
    if len(parts) >= 2:
        return "_".join(parts[:-1])
    return stem

family = extract_family_name(stem, map_id)

def savefig(name: str):
    out = PNG_DIR / name
    plt.savefig(out, bbox_inches="tight")
    print(f"✅ {out}")
    plt.close()

# 1) Vitesse
plt.figure(figsize=(10, 6))
plt.plot(summary["Depth"], summary["NPS"], marker="o", linewidth=2)
plt.title(f"Vitesse du moteur (NPS) — {stem} | {root_count} racines")
plt.xlabel("Profondeur")
plt.ylabel("Nodes per second (médiane)")
plt.grid(True, linestyle="--", alpha=0.7)
savefig(f"{stem}_nps.png")

# 2) Efficacité TT
plt.figure(figsize=(10, 6))
plt.plot(summary["Depth"], summary["HitRate"], marker="o", label="TT Hit (%)")
plt.plot(summary["Depth"], summary["CutoffRate"], marker="s", label="TT Cutoff (%)")
plt.plot(summary["Depth"], summary["OrderRate"], marker="^", label="TT Order (%)")
plt.title(f"Efficacité TT — {stem} | {root_count} racines")
plt.xlabel("Profondeur")
plt.ylabel("Pourcentage moyen")
plt.legend()
plt.grid(True, linestyle="--", alpha=0.7)
savefig(f"{stem}_tt_rates.png")

# 3) Volume de recherche
plt.figure(figsize=(10, 6))
plt.plot(summary["Depth"], summary["Nodes"], marker="o", label="Nodes")
plt.plot(summary["Depth"], summary["NewEntries"], marker="s", label="New entries")
plt.plot(summary["Depth"], summary["Overwrites"], marker="^", label="Overwrites")
plt.title(f"Volume de recherche — {stem} | {root_count} racines")
plt.xlabel("Profondeur")
plt.ylabel("Valeur médiane")
plt.legend()
plt.grid(True, linestyle="--", alpha=0.7)
savefig(f"{stem}_search_volume.png")

# 4) Pression TT
plt.figure(figsize=(10, 6))
plt.plot(summary["Depth"], summary["FillRate"], marker="o", label="Fill rate (%)")
plt.title(f"Pression sur la TT — {stem} | {root_count} racines")
plt.xlabel("Profondeur")
plt.ylabel("Pourcentage moyen")
plt.legend()
plt.grid(True, linestyle="--", alpha=0.7)
savefig(f"{stem}_tt_pressure.png")


# 5) BILAN GLOBAL
max_depth = df["Depth"].max()
total_nodes = df["Nodes"].sum()
median_nps = df["NPS"].median()
avg_hit = df["HitRate"].mean()
avg_cut = df["CutoffRate"].mean()
avg_ord = df["OrderRate"].mean()
total_overwrites = df["Overwrites"].sum()
total_rejected = df["RejectedCollisions"].sum()

# Petit helper pour formater les gros nombres (ex: 1500000 -> 1.5M)
def fmt(n):
    if n >= 1e9:
        return f"{n/1e9:.1f}G"
    if n >= 1e6:
        return f"{n/1e6:.1f}M"
    if n >= 1e3:
        return f"{n/1e3:.1f}k"
    return str(int(n))

# Affichage du bilan global
print("\n" + "=" * 80)
print(f"BILAN GLOBAL | Version: {family} | Map: {map_id} | Racines jouées: {root_count}")
median_nodes = df["Nodes"].median()
median_ms_per_mnodes = df["MsPerMNodes"].median()
print(f"MAX_D: {max_depth} | NPS: {fmt(median_nps)} | NOEUDS: {fmt(total_nodes)} | N_MED: {fmt(median_nodes)} | ms/1M: {median_ms_per_mnodes:.1f}")
print(f"TAUX TT | Hit: {avg_hit:.1f}% | Cut: {avg_cut:.1f}% | Ord: {avg_ord:.1f}%")
print(f"COLLISIONS | Écrasements (Ovw): {fmt(total_overwrites)} | Rejets (Rej): {fmt(total_rejected)}")
print("=" * 80 + "\n")

# Résumé général sous forme de csv pour A/B testing
one_line = pd.DataFrame([{
    "File": stem,
    "Version": family,
    "MapId": map_id,
    "Roots": int(root_count),
    "MaxDepth": int(df["Depth"].max()),
    "MedianNPS": int(df["NPS"].median()),
    "MedianNodes": int(df["Nodes"].median()),
    "MedianElapsedMs": round(df["ElapsedMs"].median(), 2),
    "MedianNodesPerMs": round(df["NodesPerMs"].median(), 2),
    "MedianMsPerMNodes": round(df["MsPerMNodes"].median(), 2),
    "MeanHitRate": round(df["HitRate"].mean(), 2),
    "MeanCutoffRate": round(df["CutoffRate"].mean(), 2),
    "MeanOrderRate": round(df["OrderRate"].mean(), 2),
    "MeanFillRate": round(df["FillRate"].mean(), 2),
    "MaxFillRate": int(df["FillRate"].max()),
    "TotalOverwrites": int(df["Overwrites"].sum()),
    "TotalRejectedCollisions": int(df["RejectedCollisions"].sum()),
}])

summary_csv = CSV_DIR / f"{family}_summary.csv"

if summary_csv.exists():
    old = pd.read_csv(summary_csv)
    if "File" in old.columns:
        old = old[old["File"] != stem]
    one_line = pd.concat([old, one_line], ignore_index=True)

one_line = one_line.sort_values("MapId").reset_index(drop=True)
one_line.to_csv(summary_csv, index=False)
print(f"✅ {summary_csv}")

# --- 6) PNG récapitulatif versionnel ---
summary_png = PNG_DIR / f"{family}_summary.png"
summary_df = one_line.copy()

summary_df["MedianNPS_M"] = summary_df["MedianNPS"] / 1_000_000.0
summary_df["MedianNodes_K"] = summary_df["MedianNodes"] / 1_000.0
summary_df["TotalOverwrites_M"] = summary_df["TotalOverwrites"] / 1_000_000.0

fig, axes = plt.subplots(2, 2, figsize=(18, 10))
fig.suptitle(f"Résumé A/B Testing — {family}", fontsize=16, fontweight="bold")

# 1) Vitesse
ax = axes[0, 0]
ax.bar(summary_df["MapId"], summary_df["MedianNPS_M"])
ax.set_title("Vitesse médiane")
ax.set_ylabel("NPS (millions)")
ax.grid(True, linestyle="--", alpha=0.7, axis="y")
ax.tick_params(axis="x", rotation=35)

# 2) Taux TT
ax = axes[0, 1]
x = range(len(summary_df))
w = 0.25
ax.bar([i - w for i in x], summary_df["MeanHitRate"], width=w, label="Hit %")
ax.bar(x, summary_df["MeanCutoffRate"], width=w, label="Cutoff %")
ax.bar([i + w for i in x], summary_df["MeanOrderRate"], width=w, label="Order %")
ax.set_title("Qualité TT")
ax.set_ylabel("Pourcentage")
ax.set_xticks(list(x))
ax.set_xticklabels(summary_df["MapId"], rotation=35)
ax.legend()
ax.grid(True, linestyle="--", alpha=0.7, axis="y")

# 3) Coût / volume
# 3) Volume / rendement
ax = axes[1, 0]
summary_df["MedianNodes_K"] = summary_df["MedianNodes"] / 1_000.0

bars = ax.bar(summary_df["MapId"], summary_df["MedianNodes_K"], label="Nodes médians (k)")
ax.set_title("Volume / rendement")
ax.set_ylabel("Nodes médians (milliers)")
ax.grid(True, linestyle="--", alpha=0.7, axis="y")
ax.tick_params(axis="x", rotation=35)

ax2 = ax.twinx()
line = ax2.plot(
    summary_df["MapId"],
    summary_df["MedianMsPerMNodes"],
    color="orange",
    marker="o",
    linewidth=2,
    label="ms / 1M nodes"
)
ax2.set_ylabel("ms par million de nœuds")

handles1, labels1 = ax.get_legend_handles_labels()
handles2, labels2 = ax2.get_legend_handles_labels()
ax.legend(handles1 + handles2, labels1 + labels2, loc="upper left")

# 4) Pression TT
ax = axes[1, 1]
ax.bar([i - w/2 for i in x], summary_df["MeanFillRate"], width=w, label="Fill moyen %")
ax.bar([i + w/2 for i in x], summary_df["MaxFillRate"], width=w, label="Fill max %")
ax.set_title("Pression TT")
ax.set_ylabel("Pourcentage")
ax.set_xticks(list(x))
ax.set_xticklabels(summary_df["MapId"], rotation=35)
ax.legend()
ax.grid(True, linestyle="--", alpha=0.7, axis="y")

plt.tight_layout(rect=[0, 0, 1, 0.95])
plt.savefig(summary_png, bbox_inches="tight")
print(f"✅ {summary_png}")
plt.close()

if SHOW:
    plt.show()

# rm -f logs/hash/csv/*_summary.csv logs/hash/png/*_summary.png && \
# for f in logs/hash/csv/*.csv; do
#   case "$f" in
#     *_summary.csv) ;;
#     *) python3 scripts/hash.py "$f" ;;
#   esac
# done