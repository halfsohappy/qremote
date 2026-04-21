// Thin C++ bridge that feeds inbound UDP payloads to the vendored MicroOsc
// parser and hands each decoded message up to C code via a callback.
//
// MicroOsc is a C++ lib with abstract transport hooks; we subclass it as
// a passive (receive-only) parser — the overrides are no-ops because we
// never call its send API here.

#include "osc_bridge.h"

#include <MicroOsc.h>
#include <MicroOscMessage.h>

#include <stdio.h>
#include <string.h>

namespace {

class PassiveOsc : public MicroOsc {
public:
    PassiveOsc() : MicroOsc(nullptr) {}
protected:
    void transportBegin() override {}
    void transportEnd() override {}
    bool transportReady() override { return false; }
public:
    void onOscMessageReceived(MicroOscCallback) override {}
    void onOscMessageReceived(MicroOscCallbackWithSource) override {}
};

struct Ctx {
    osc_handler_t cb;
    void *user;
};

// MicroOsc's parseMessages takes a plain-function callback with no user
// pointer, so we stash context in a thread-local and read it back inside
// the trampoline. Parse is synchronous and single-threaded per call.
thread_local Ctx g_ctx = { nullptr, nullptr };

void format_args(MicroOscMessage &msg, char *out, size_t out_sz) {
    const char *tags = msg.getTypeTags();
    size_t off = 0;
    out[0] = '\0';
    if (!tags) return;

    for (const char *t = tags; *t && off + 1 < out_sz; ++t) {
        int n = 0;
        if (off > 0) { out[off++] = ' '; out[off] = '\0'; if (off + 1 >= out_sz) break; }

        switch (*t) {
        case 'i':
            n = snprintf(out + off, out_sz - off, "%ld", (long)msg.nextAsInt());
            break;
        case 'f':
            n = snprintf(out + off, out_sz - off, "%g", (double)msg.nextAsFloat());
            break;
        case 'd':
            n = snprintf(out + off, out_sz - off, "%g", msg.nextAsDouble());
            break;
        case 's': {
            const char *s = msg.nextAsString();
            n = snprintf(out + off, out_sz - off, "\"%s\"", s ? s : "");
            break;
        }
        case 'T': n = snprintf(out + off, out_sz - off, "true");  break;
        case 'F': n = snprintf(out + off, out_sz - off, "false"); break;
        case 'N': n = snprintf(out + off, out_sz - off, "nil");   break;
        case 'I': n = snprintf(out + off, out_sz - off, "bang");  break;
        case 'b': {
            const uint8_t *blob = nullptr;
            uint32_t blen = msg.nextAsBlob(&blob);
            n = snprintf(out + off, out_sz - off, "<blob %u>", (unsigned)blen);
            break;
        }
        default:
            // Unknown/unsupported tag — advance isn't safe since MicroOsc's
            // nextAs* don't cover it. Stop summarising and note the tag.
            n = snprintf(out + off, out_sz - off, "?%c…", *t);
            off += (n < 0 ? 0 : (size_t)n);
            if (off >= out_sz) off = out_sz - 1;
            out[off] = '\0';
            return;
        }
        if (n < 0) break;
        off += (size_t)n;
        if (off >= out_sz) { off = out_sz - 1; break; }
    }
    out[off] = '\0';
}

void trampoline(MicroOscMessage &msg) {
    if (!g_ctx.cb) return;
    osc_parsed_t parsed;
    parsed.address  = msg.getOscAddress();
    parsed.typetags = msg.getTypeTags();
    format_args(msg, parsed.summary, sizeof(parsed.summary));
    g_ctx.cb(&parsed, g_ctx.user);
}

} // namespace

extern "C" void osc_parse(uint8_t *buffer, size_t len, osc_handler_t cb, void *user) {
    if (!cb || !buffer || len == 0) return;
    static PassiveOsc osc;
    g_ctx = { cb, user };
    osc.parseMessages(&trampoline, buffer, len);
    g_ctx = { nullptr, nullptr };
}
