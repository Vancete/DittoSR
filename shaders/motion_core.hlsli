// The motion pass' checks for one render pixel (motion.hlsl, and the upscale at 1x - FUSED_MOTION): its uv, its convert
// pass texel (mda4), its encoded luma and its 3x3's range (lc, lmn, lmx: 0, 1, 0 when neither the reactive test nor
// the envelope is on). ax = x the motion's change against its neighbours' (rj), y the disocclusion, z the derived
// reactive mask, w the luma; env = the luma envelope. The includer names the textures: MC_MDA (this frame's convert
// pass), MC_PREVMDA (the last frame's), MC_PREVMOTION (the last frame's ax), MC_PREVENV (the last frame's envelope).
void motionCore(float2 uv, float4 mda4, float lc, float lmn, float lmx, out float4 ax, out float2 env) {
    float2 puv = uv - 0.5 * mda4.xy;
    bool off = any(puv < 0.0) || any(puv > 1.0);
    if (reset > 0.5 || off) {
        ax = float4(0.0, 0.0, 0.0, lc);
        env = float2(lc, lc);
        return;
    }
    // (the previous frame's motion where this pixel was: changed by over half a pixel more than its neighbours' -
    // another object's - less history, a narrower box. Against the neighbours: the camera speeding up or slowing down,
    // a hand on the mouse, changes every pixel's motion alike and cut the whole history; another object's edge changes it
    // here and not a little way off. The neighbours as far as the change itself (in output pixels, as when the upscale
    // did it), so a revealed strip's lie outside it)
    float4 pm = MC_PREVMDA.SampleLevel(linearClamp, puv, 0.0);
    float2 dlt = 0.5 * (mda4.xy - pm.xy) * os;
    float rad = clamp(length(dlt), 2.0, 16.0), pdif = 0.0;
    [unroll] for (int n = 0; n < 4; n++) {
        float2 nuv = uv + float2(n == 0 ? rad : n == 1 ? -rad : 0.0, n == 2 ? rad : n == 3 ? -rad : 0.0) * osRcp;
        float2 nm = MC_MDA.SampleLevel(linearClamp, nuv, 0.0).xy;
        float2 npm = MC_PREVMDA.SampleLevel(linearClamp, nuv - 0.5 * nm, 0.0).xy;
        pdif = max(pdif, length(dlt - 0.5 * (nm - npm) * os));
    }
    float rj = smoothstep(0.5, 2.0, pdif);
    // (the disocclusion: where the pixel was, the previous frame's surfaces - even the farthest of the four
    // around it - nearer than this one, and moving otherwise than it (their motion: the previous frame's convert pass
    // took the nearest's): it was hidden behind something that moved away. A turn of the camera, a still scene, or a
    // thin blade the jitter hits one frame and misses the next keep the history - GSR 2's depth clip cut it at every
    // depth edge as soon as anything moved, the depth alone at every sub-pixel blade: the grass's noise)
    float4 pn = MC_PREVMDA.GatherAlpha(linearClamp, puv);
    float np = min(min(pn.x, pn.y), min(pn.z, pn.w)), nc = mda4.w;
    float nearer = np > 1e-7 ? smoothstep(0.03, 0.15, 1.0 - nc / np) : 0.0;
    float dz = nearer * smoothstep(0.25, 1.0, pdif);
    // (the derived reactive mask, where the game gives none: the luma here against the previous frame's where this pixel
    // was, beyond what the 3x3's own spread explains - the jitter's aliasing of an edge or a blade stays within it; a
    // particle, smoke, a transparency or a flash on a flat area does not. Its history then counts less: no trail behind
    // what moves without motion vectors of its own)
    float rv = 0.0;
    if (autoReactive > 0.0) {
        float lp = MC_PREVMOTION.SampleLevel(linearClamp, puv, 0.0).w;
        rv = autoReactive * saturate((abs(lc - lp) - 2.0 * (lmx - lmn) - 0.06) * 5.0);
    }
    // (the envelope: the range this pixel's luma has taken over the last frames - a decaying min / max, carried where
    // the pixel went. A static pixel's jittered samples bounce within it, however aliased; a luma outside it by more
    // than the tolerance is something new there - what moved with no motion vector of its own, arriving: the samples'
    // box, widened by the arrival itself, does not cut the history; this does, in the first frame)
    env = float2(lc, lc);
    if (envGain > 0.0) {
        float2 pe = MC_PREVENV.SampleLevel(linearClamp, puv, 0.0);
        float ex = max(max(lc - pe.y, pe.x - lc), 0.0);
        float still = envRest > 0.5 ? 1.0 - saturate((length(0.5 * mda4.xy * os) - 0.5) / 1.5) : 1.0;
        float wdt = pe.y - pe.x;
        rv = max(rv, min(saturate((ex - envTol - envRel * wdt) * envGain), envCap) * still);
        env = float2(min(lc, pe.x + envDecay * wdt), max(lc, pe.y - envDecay * wdt));
    }
    ax = float4(rj, dz, rv, lc);
}
