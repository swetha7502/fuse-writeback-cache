import streamlit as st
import plotly.graph_objects as go
import plotly.express as px
import pandas as pd
import re, os, subprocess

st.set_page_config(page_title="FUSE Cache Dashboard", layout="wide")

# ── Parse fio terse output ─────────────────────────────────────────
# fio --output-format=terse produces semicolon-separated lines.
# Field indices (version 3):
#   3  = job name
#   6  = read IOPS
#   7  = read BW KB/s
#   48 = write IOPS
#   49 = write BW KB/s
#   40 = read lat mean (usec)
#   79 = write lat mean (usec)

def parse_fio_terse(path):
    results = []
    if not os.path.exists(path):
        return results
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("3;fio"):
                continue
            parts = line.split(";")
            if len(parts) < 85:
                continue
            try:
                results.append({
                    "job":         parts[3],
                    "read_iops":   int(float(parts[6])),
                    "read_bw_mbs": round(int(parts[7]) / 1024, 1),
                    "write_iops":  int(float(parts[48])),
                    "write_bw_mbs":round(int(parts[49]) / 1024, 1),
                    "read_lat_ms": round(float(parts[40]) / 1000, 2),
                    "write_lat_ms":round(float(parts[79]) / 1000, 2),
                })
            except (ValueError, IndexError):
                continue
    return results

# ── Load data ──────────────────────────────────────────────────────
RESULTS = "../results"
wb_data = parse_fio_terse(f"{RESULTS}/writeback.txt")
wt_data = parse_fio_terse(f"{RESULTS}/writethrough.txt")

# Parse stats line from fuse output (printed on unmount)
def parse_stats(path):
    stats = {"hits": 0, "misses": 0, "evictions": 0, "hit_rate": 0.0}
    if not os.path.exists(path):
        return stats
    with open(path) as f:
        text = f.read()
    m = re.search(r"hits=(\d+)\s+misses=(\d+)\s+evictions=(\d+)", text)
    if m:
        h, ms, ev = int(m.group(1)), int(m.group(2)), int(m.group(3))
        stats = {"hits": h, "misses": ms, "evictions": ev,
                 "hit_rate": round(100*h/(h+ms), 1) if h+ms > 0 else 0}
    return stats

wb_stats = parse_stats(f"{RESULTS}/writeback_stats.txt")

# ── Fallback demo data (shown before benchmarks are run) ───────────
DEMO = not (wb_data and wt_data)
if DEMO:
    wb_data = [
        {"job":"seqwrite","write_bw_mbs":312,"write_iops":80000,"write_lat_ms":0.13,"read_bw_mbs":0,"read_iops":0,"read_lat_ms":0},
        {"job":"randwrite","write_bw_mbs":276,"write_iops":71000,"write_lat_ms":0.14,"read_bw_mbs":0,"read_iops":0,"read_lat_ms":0},
        {"job":"seqread","read_bw_mbs":410,"read_iops":105000,"read_lat_ms":0.09,"write_bw_mbs":0,"write_iops":0,"write_lat_ms":0},
    ]
    wt_data = [
        {"job":"seqwrite","write_bw_mbs":76,"write_iops":19500,"write_lat_ms":0.52,"read_bw_mbs":0,"read_iops":0,"read_lat_ms":0},
        {"job":"randwrite","write_bw_mbs":43,"write_iops":11200,"write_lat_ms":0.91,"read_bw_mbs":0,"read_iops":0,"read_lat_ms":0},
        {"job":"seqread","read_bw_mbs":95,"read_iops":24300,"read_lat_ms":0.41,"write_bw_mbs":0,"write_iops":0,"write_lat_ms":0},
    ]
    wb_stats = {"hits":142,"misses":8,"evictions":0,"hit_rate":94.7}

def find_job(data, keyword):
    for d in data:
        if keyword in d["job"]:
            return d
    return {}

wb_seq  = find_job(wb_data, "seqwrite")
wt_seq  = find_job(wt_data, "seqwrite")
wb_rand = find_job(wb_data, "randwrite")
wt_rand = find_job(wt_data, "randwrite")
wb_read = find_job(wb_data, "seqread")
wt_read = find_job(wt_data, "seqread")

BLUE = "#185FA5"
GRAY = "#888780"

# ── UI ─────────────────────────────────────────────────────────────
st.title("FUSE write-back cache — performance dashboard")
if DEMO:
    st.info("No benchmark results found in `../results/`. Showing demo data. Run `./benchmark.sh` first.", icon="ℹ")

# ── Top metrics ────────────────────────────────────────────────────
c1, c2, c3, c4, c5 = st.columns(5)
speedup = round(wb_seq.get("write_bw_mbs",0) / max(wt_seq.get("write_bw_mbs",1), 1), 1)
c1.metric("Write-back seq write",  f'{wb_seq.get("write_bw_mbs",0)} MB/s')
c2.metric("Write-through seq write", f'{wt_seq.get("write_bw_mbs",0)} MB/s')
c3.metric("Speedup (seq write)",   f"{speedup}x",   delta=f"+{speedup-1:.1f}x over write-through")
c4.metric("Cache hit rate",        f'{wb_stats["hit_rate"]}%', delta=f'{wb_stats["hits"]} hits / {wb_stats["misses"]} misses')
c5.metric("Evictions",             str(wb_stats["evictions"]))

st.divider()

# ── Row 1: throughput + latency ────────────────────────────────────
col1, col2 = st.columns(2)

with col1:
    st.subheader("Write throughput (MB/s)")
    fig = go.Figure()
    jobs    = ["Sequential write", "Random write"]
    wb_vals = [wb_seq.get("write_bw_mbs",0), wb_rand.get("write_bw_mbs",0)]
    wt_vals = [wt_seq.get("write_bw_mbs",0), wt_rand.get("write_bw_mbs",0)]
    fig.add_bar(name="Write-back",    x=jobs, y=wb_vals, marker_color=BLUE)
    fig.add_bar(name="Write-through", x=jobs, y=wt_vals, marker_color=GRAY)
    fig.update_layout(barmode="group", yaxis_title="MB/s",
                      legend=dict(orientation="h", y=1.12),
                      margin=dict(t=10,b=0,l=0,r=0), height=280)
    st.plotly_chart(fig, use_container_width=True)

with col2:
    st.subheader("Write latency (ms, lower is better)")
    fig2 = go.Figure()
    wb_lat = [wb_seq.get("write_lat_ms",0), wb_rand.get("write_lat_ms",0)]
    wt_lat = [wt_seq.get("write_lat_ms",0), wt_rand.get("write_lat_ms",0)]
    fig2.add_bar(name="Write-back",    x=jobs, y=wb_lat, marker_color=BLUE)
    fig2.add_bar(name="Write-through", x=jobs, y=wt_lat, marker_color=GRAY)
    fig2.update_layout(barmode="group", yaxis_title="ms",
                       legend=dict(orientation="h", y=1.12),
                       margin=dict(t=10,b=0,l=0,r=0), height=280)
    st.plotly_chart(fig2, use_container_width=True)

# ── Row 2: IOPS + read comparison ─────────────────────────────────
col3, col4 = st.columns(2)

with col3:
    st.subheader("IOPS (4K blocks)")
    fig3 = go.Figure()
    wb_iops = [wb_seq.get("write_iops",0), wb_rand.get("write_iops",0)]
    wt_iops = [wt_seq.get("write_iops",0), wt_rand.get("write_iops",0)]
    fig3.add_bar(name="Write-back",    x=jobs, y=wb_iops, marker_color=BLUE)
    fig3.add_bar(name="Write-through", x=jobs, y=wt_iops, marker_color=GRAY)
    fig3.update_layout(barmode="group", yaxis_title="IOPS",
                       legend=dict(orientation="h", y=1.12),
                       margin=dict(t=10,b=0,l=0,r=0), height=280)
    st.plotly_chart(fig3, use_container_width=True)

with col4:
    st.subheader("Read throughput after write (MB/s)")
    st.caption("Write-back serves reads from in-memory cache; write-through must re-read from disk.")
    fig4 = go.Figure()
    modes   = ["Write-back\n(cache hit)", "Write-through\n(disk read)"]
    rd_vals = [wb_read.get("read_bw_mbs",0), wt_read.get("read_bw_mbs",0)]
    colors  = [BLUE, GRAY]
    fig4.add_bar(x=modes, y=rd_vals, marker_color=colors, showlegend=False)
    fig4.update_layout(yaxis_title="MB/s",
                       margin=dict(t=10,b=0,l=0,r=0), height=280)
    st.plotly_chart(fig4, use_container_width=True)

# ── Cache hit rate curve ───────────────────────────────────────────
st.subheader("Cache hit rate vs cache size")
st.caption("Simulated — re-run benchmark.sh with --cache-mb=N to get real data points.")
sizes    = [8, 16, 32, 64, 128, 256]
hit_rate = [41, 63, 79, 91, 97, 99]
fig5 = px.line(x=sizes, y=hit_rate, markers=True,
               labels={"x":"Cache size (MB)", "y":"Hit rate (%)"},
               color_discrete_sequence=[BLUE])
fig5.update_layout(margin=dict(t=10,b=0,l=0,r=0), height=240, yaxis_range=[0,100])
st.plotly_chart(fig5, use_container_width=True)

# ── Summary table ──────────────────────────────────────────────────
st.subheader("Raw numbers")
summary = pd.DataFrame({
    "Metric":         ["Seq write (MB/s)", "Rand write (MB/s)", "Seq write IOPS", "Rand write IOPS",
                       "Seq write lat (ms)", "Rand write lat (ms)", "Read after write (MB/s)"],
    "Write-back":     [wb_seq.get("write_bw_mbs","-"), wb_rand.get("write_bw_mbs","-"),
                       wb_seq.get("write_iops","-"),   wb_rand.get("write_iops","-"),
                       wb_seq.get("write_lat_ms","-"), wb_rand.get("write_lat_ms","-"),
                       wb_read.get("read_bw_mbs","-")],
    "Write-through":  [wt_seq.get("write_bw_mbs","-"), wt_rand.get("write_bw_mbs","-"),
                       wt_seq.get("write_iops","-"),   wt_rand.get("write_iops","-"),
                       wt_seq.get("write_lat_ms","-"), wt_rand.get("write_lat_ms","-"),
                       wt_read.get("read_bw_mbs","-")],
})
st.dataframe(summary, use_container_width=True, hide_index=True)