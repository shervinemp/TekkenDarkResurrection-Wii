/* The SAS mixer as it was before the exact rewrite: the plain algorithm, every source sample through
   ref_next_sample() and every guest byte through the guest accessors.  It is included by sas.c only for
   -DEXP_SASCHK (core_mix() mixes a copy of the voices with it into host buffers and compares the output
   and every voice with what the fast code did) and -DEXP_ABSND (the mixer alternates between the two so one
   run times both).  The shared pieces (the envelope, the tables, clamp16) are those of sas.c. */

static void ref_vag_decode_block(Voice *v) {
    if (v->block == v->nblocks - 1) { v->ended = 1; return; }
    uint32_t p = v->read;
    int hdr = mem_r8(p), flags = mem_r8(p + 1);
    int shift = hdr & 15, pred = hdr >> 4;
    if (flags == 7) { v->ended = 1; return; }
    if (flags == 6) v->loop_block = v->block + 1;
    else if (flags == 3 && v->loop_enabled) v->loop_next = 1;
    int c1 = k_filter[pred][0], c2 = -k_filter[pred][1], s1 = v->s1, s2 = v->s2;
    for (int i = 0; i < 28; i += 2) {
        int d = mem_r8(p + 2 + (uint32_t)i / 2);
        int a = (int16_t)((d & 0xF) << 12) >> shift, b = (int16_t)((d & 0xF0) << 8) >> shift;
        s2 = clamp16(a + ((s1 * c1 + s2 * c2) >> 6));
        s1 = clamp16(b + ((s2 * c1 + s1 * c2) >> 6));
        v->samples[i] = (int16_t)s2;
        v->samples[i + 1] = (int16_t)s1;
    }
    v->s1 = s1;
    v->s2 = s2;
    v->cur = 0;
    v->block++;
    v->read = p + 16;
}

static int16_t ref_next_sample(Voice *v) {
    if (v->noise) {
        v->noise_state = v->noise_state * 1103515245u + 12345u;
        return (int16_t)(v->noise_state >> 16);
    }
    if (v->ended) return 0;
    if (v->cur == 28) {
        if (v->loop_next && v->loop_block >= 0) {
            v->read = v->vag_addr + 16 * (uint32_t)v->loop_block;
            v->block = v->loop_block - 1;
            v->loop_next = 0;
        }
        ref_vag_decode_block(v);
        if (v->ended) return 0;
    }
    return v->samples[v->cur++];
}

static void ref_mix_voice(Voice *v, int32_t *mix, int32_t *send) {
    const int grain = g_grain, pitch = v->pitch, vol_l = v->vol_l, vol_r = v->vol_r, eff_l = v->eff_l, eff_r = v->eff_r;
    uint32_t frac = v->frac;
    int prev = v->prev, next = v->next;
    Envelope e = v->env;
    for (int i = 0; i < grain; i++) {
        int s = prev + (((next - prev) * (int)(frac & 0xFFF)) >> 12);
        frac += (uint32_t)pitch;
        while (frac >= PITCH_BASE) {
            frac -= PITCH_BASE;
            prev = next;
            next = ref_next_sample(v);
        }
        int64_t h = e.height > ENV_MAX ? ENV_MAX : e.height < 0 ? 0 : e.height;
        if (!(e.state == ST_SUSTAIN && e.rate[2] == 0 && e.type[2] <= CURVE_EXP_DEC && e.height > 0)) env_step(&e);
        int env = (int)(((uint32_t)h + (1u << 14)) >> 15);
        s = (s * env + (1 << 14)) >> 15;
        mix[i * 2] += (s * vol_l) >> 12;
        mix[i * 2 + 1] += (s * vol_r) >> 12;
        send[i * 2] += (s * eff_l) >> 12;
        send[i * 2 + 1] += (s * eff_r) >> 12;
    }
    v->frac = frac;
    v->prev = prev;
    v->next = next;
    v->env = e;
    if (!v->noise && v->ended) set_state(&v->env, ST_OFF), v->env.height = 0;
    if (v->env.state == ST_OFF) v->playing = v->on = 0;
}

/* The whole call on the voices `vs` with the buffers `mix` and `send` (both zero on entry and left zero
   again): the output goes to `hout` (interleaved stereo, host order) or, when that is NULL, to guest memory. */
static void ref_core_mix(Voice *vs, int32_t *mix, int32_t *send, uint32_t out, uint32_t in, int lvol, int rvol, int16_t *hout) {
    for (int i = 0; i < NVOICES; i++)
        if (vs[i].playing && !vs[i].paused) ref_mix_voice(&vs[i], mix, send);
    for (int i = 0; i < g_grain * 2; i += 2) {
        int l = 0, r = 0;
        if (in) {
            l = ((int16_t)mem_r16(in + (uint32_t)i * 2) * lvol) >> 12;
            r = ((int16_t)mem_r16(in + (uint32_t)i * 2 + 2) * rvol) >> 12;
        }
        if (g_dry) { l += mix[i]; r += mix[i + 1]; }
        if (g_wet) { l += send[i] / 2; r += send[i + 1] / 2; }
        if (hout) {
            hout[i] = clamp16(l);
            hout[i + 1] = clamp16(r);
        } else {
            mem_w16(out + (uint32_t)i * 2, (uint16_t)clamp16(l));
            mem_w16(out + (uint32_t)i * 2 + 2, (uint16_t)clamp16(r));
        }
    }
    memset(mix, 0, sizeof(int32_t) * (size_t)g_grain * 2);
    memset(send, 0, sizeof(int32_t) * (size_t)g_grain * 2);
}
