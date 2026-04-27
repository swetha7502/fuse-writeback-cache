import streamlit as st
import plotly.graph_objects as go
import plotly.express as px
import pandas as pd
import re, json
from pathlib import Path

st.set_page_config(page_title="FUSE Cache Dashboard", layout="wide")

RESULTS = Path(__file__).resolve().parent.parent / "results"

BLUE = "#185FA5"
GRAY = "#888780"


def parse_fio_json(path):
    if not Path(path).exists():
        return {}

    with open(path) as f:
        data = json.load(f)

    job = data["jobs"][0]
    write = job["write"]

    return {
        "job": job["jobname"],
        "write_iops": int(write["iops"]),
        "write_bw_mbs": round(write["bw"] / 1024, 1),
        "write_lat_ms": round(write["lat_ns"]["mean"] / 1_000_000, 3),
    }


def parse_stats(path):
    stats = {"hits": 0, "misses": 0, "evictions": 0, "hit_rate": 0.0}

    if not Path(path).exists():
        return stats

    with open(path) as f:
        text = f.read()

    m = re.search(r"hits=(\d+)\s+misses=(\d+)\s+evictions=(\d+)", text)
    if m:
        h, ms, ev = int(m.group(1)), int(m.group(2)), int(m.group(3))
        stats = {
            "hits": h,
            "misses": ms,
            "evictions": ev,
            "hit_rate": round(100 * h / (h + ms), 1) if h + ms > 0 else 0,
        }

    return stats


wb_seq = parse_fio_json(RESULTS / "writeback_seqwrite.json")
wt_seq = parse_fio_json(RESULTS / "writethrough_seqwrite.json")
wb_rand = parse_fio_json(RESULTS / "writeback_randwrite.json")
wt_rand = parse_fio_json(RESULTS / "writethrough_randwrite.json")
wb_stats = parse_stats(RESULTS / "writeback_stats.txt")

DEMO = not (wb_seq and wt_seq and wb_rand and wt_rand)

if DEMO:
    wb_seq = {"job": "seqwrite", "write_bw_mbs": 139.4, "write_iops": 35694, "write_lat_ms": 0.028}
    wt_seq = {"job": "seqwrite", "write_bw_mbs": 158.0, "write_iops": 40454, "write_lat_ms": 0.024}
    wb_rand = {"job": "randwrite", "write_bw_mbs": 179.4, "write_iops": 45918, "write_lat_ms": 0.021}
    wt_rand = {"job": "randwrite", "write_bw_mbs": 164.5, "write_iops": 42124, "write_lat_ms": 0.023}


st.title("FUSE write-back cache — performance dashboard")

if DEMO:
    st.info(
        "No benchmark JSON results found in `../results/`. Showing fallback data. Run `./benchmark.sh` first.",
        icon="ℹ",
    )

c1, c2, c3, c4, c5 = st.columns(5)

seq_speedup = round(
    wb_seq.get("write_bw_mbs", 0) / max(wt_seq.get("write_bw_mbs", 1), 1), 2
)
rand_speedup = round(
    wb_rand.get("write_bw_mbs", 0) / max(wt_rand.get("write_bw_mbs", 1), 1), 2
)

c1.metric("Write-back seq write", f'{wb_seq.get("write_bw_mbs", 0)} MB/s')
c2.metric("Write-through seq write", f'{wt_seq.get("write_bw_mbs", 0)} MB/s')
c3.metric("Seq speedup", f"{seq_speedup}x")
c4.metric("Random speedup", f"{rand_speedup}x")
c5.metric("Evictions", str(wb_stats["evictions"]))

st.divider()

col1, col2 = st.columns(2)

jobs = ["Sequential write", "Random write"]

with col1:
    st.subheader("Write throughput (MB/s)")
    fig = go.Figure()
    fig.add_bar(
        name="Write-back",
        x=jobs,
        y=[wb_seq["write_bw_mbs"], wb_rand["write_bw_mbs"]],
        marker_color=BLUE,
    )
    fig.add_bar(
        name="Write-through",
        x=jobs,
        y=[wt_seq["write_bw_mbs"], wt_rand["write_bw_mbs"]],
        marker_color=GRAY,
    )
    fig.update_layout(
        barmode="group",
        yaxis_title="MB/s",
        legend=dict(orientation="h", y=1.12),
        margin=dict(t=10, b=0, l=0, r=0),
        height=280,
    )
    st.plotly_chart(fig, use_container_width=True)

with col2:
    st.subheader("Write latency (ms, lower is better)")
    fig2 = go.Figure()
    fig2.add_bar(
        name="Write-back",
        x=jobs,
        y=[wb_seq["write_lat_ms"], wb_rand["write_lat_ms"]],
        marker_color=BLUE,
    )
    fig2.add_bar(
        name="Write-through",
        x=jobs,
        y=[wt_seq["write_lat_ms"], wt_rand["write_lat_ms"]],
        marker_color=GRAY,
    )
    fig2.update_layout(
        barmode="group",
        yaxis_title="ms",
        legend=dict(orientation="h", y=1.12),
        margin=dict(t=10, b=0, l=0, r=0),
        height=280,
    )
    st.plotly_chart(fig2, use_container_width=True)

col3, col4 = st.columns(2)

with col3:
    st.subheader("IOPS (4K blocks)")
    fig3 = go.Figure()
    fig3.add_bar(
        name="Write-back",
        x=jobs,
        y=[wb_seq["write_iops"], wb_rand["write_iops"]],
        marker_color=BLUE,
    )
    fig3.add_bar(
        name="Write-through",
        x=jobs,
        y=[wt_seq["write_iops"], wt_rand["write_iops"]],
        marker_color=GRAY,
    )
    fig3.update_layout(
        barmode="group",
        yaxis_title="IOPS",
        legend=dict(orientation="h", y=1.12),
        margin=dict(t=10, b=0, l=0, r=0),
        height=280,
    )
    st.plotly_chart(fig3, use_container_width=True)

with col4:
    st.subheader("Cache hit rate")
    st.metric(
        "Observed hit rate",
        f'{wb_stats["hit_rate"]}%',
        delta=f'{wb_stats["hits"]} hits / {wb_stats["misses"]} misses',
    )
    st.caption("Shown when writeback_stats.txt is generated during unmount.")

st.subheader("Cache hit rate vs cache size")
st.caption("Simulated curve unless benchmarks are repeated with different --cache-mb values.")
sizes = [8, 16, 32, 64, 128, 256]
hit_rate = [41, 63, 79, 91, 97, 99]
fig5 = px.line(
    x=sizes,
    y=hit_rate,
    markers=True,
    labels={"x": "Cache size (MB)", "y": "Hit rate (%)"},
    color_discrete_sequence=[BLUE],
)
fig5.update_layout(margin=dict(t=10, b=0, l=0, r=0), height=240, yaxis_range=[0, 100])
st.plotly_chart(fig5, use_container_width=True)

st.subheader("Raw numbers")
summary = pd.DataFrame(
    {
        "Metric": [
            "Seq write (MB/s)",
            "Rand write (MB/s)",
            "Seq write IOPS",
            "Rand write IOPS",
            "Seq write latency (ms)",
            "Rand write latency (ms)",
        ],
        "Write-back": [
            wb_seq["write_bw_mbs"],
            wb_rand["write_bw_mbs"],
            wb_seq["write_iops"],
            wb_rand["write_iops"],
            wb_seq["write_lat_ms"],
            wb_rand["write_lat_ms"],
        ],
        "Write-through": [
            wt_seq["write_bw_mbs"],
            wt_rand["write_bw_mbs"],
            wt_seq["write_iops"],
            wt_rand["write_iops"],
            wt_seq["write_lat_ms"],
            wt_rand["write_lat_ms"],
        ],
    }
)

st.dataframe(summary, use_container_width=True, hide_index=True)