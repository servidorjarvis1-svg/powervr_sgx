"""docs/GL3.md test H4: a vertex program stores its attributes with st.a.d at
BUF + 16 * index (sgx_hwt vs / vcount / dump). Writes h4.txt; run it with
sgx_hwt. Vertex i lands in dwords 4i+1..4i+4 (ISA.md: base + 4 * (#i + 1))."""
VS_HEAD = ["phas.i @p1, #1", "nop [nosched]", "test.and.ne p0, #0, #0 [skipinv nodst=0]",
           "(p0) ld.a.d pa0, pa0, #0 [skipinv rpt=4]", "wdf",
           "limm pa3, #0x3f800000 [skipinv end]", "p1:", "phas.i @p2, #0 [wait=2]"]
VS_TAIL = ["mov o0, pa0 [skipinv rcnt rpt=4]", "emit.t3 r0, #0, #0 [end f12 f10]", "p2:",
           "phas.i #0, #0 [wait=7]", "emit.t1 r0, #0, #0 [end f12 f10 f3_20]"]
STORE = ["limm r2, BUF [skipinv]", "ima32 r1, pa2, #16, r2 [skipinv]",
         "limm r3, #0x4834ab00 [skipinv]",
         "st.a.d r1, #0, pa0 [skipinv]", "st.a.d r1, #1, pa1 [skipinv]",
         "st.a.d r1, #2, pa2 [skipinv]", "st.a.d r1, #3, r3 [skipinv]"]
FS = ["limm r0, #0x11223344 [skipinv]", "mov o0, r0 [skipinv end mask=1]"]
out = []
def t(name, vs, vcount, dump):
    out.append(f"test {name}")
    if vs is not None:
        out.append(f"vcount {vcount}")
        out.extend("vs " + l for l in vs)
    out.append(f"dump {dump}")
    out.extend(FS + ["end"])
t("h4_ctrl_builtin", None, 3, 16)
t("h4_ctrl_vs_nost", VS_HEAD + VS_TAIL, 3, 16)
t("h4_st3", VS_HEAD + STORE + VS_TAIL, 3, 16)
t("h4_st60", VS_HEAD + STORE + VS_TAIL, 60, 244)
open("h4.txt", "w").write("\n".join(out) + "\n")
