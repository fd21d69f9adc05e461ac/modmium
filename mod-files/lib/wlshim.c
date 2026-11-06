/*
 * wlshim: LD_PRELOAD shim that lets Wayland clients written for newer
 * protocol versions talk to Exo (ChromeOS), which is stuck on older ones.
 *
 * What it does:
 *   - advertises selected globals at a higher version than Exo offers and
 *     downgrades the version on the wire when the client binds them
 *   - rewrites wl_surface.damage_buffer (v4) into wl_surface.damage
 *   - fakes zwp_linux_dmabuf_feedback_v1 (dmabuf v4) locally so Mesa can
 *     find the render node and use dmabuf buffers instead of wl_shm
 *   - optionally caps the commit rate of buffer-bearing surfaces
 *   - optionally reports the compositor's logical screen size (xdg_output) as
 *     the wl_output mode, so Wine's monitor rectangle matches what the
 *     compositor can actually show (see WLSHIM_LOGICAL_MODE)
 *
 * Environment:
 *   WLSHIM_FAKE          name=version,... globals to upgrade
 *                        (default: wl_compositor=4)
 *   WLSHIM_RENDER_NODE   render node for the fake feedback
 *                        (default: /dev/dri/renderD128)
 *   WLSHIM_FPS           frame cap per surface, 0 or unset = off
 *   WLSHIM_LOGICAL_MODE  1 = replace the wl_output current mode by the
 *                        xdg_output logical size. Wine builds its monitor
 *                        rectangle from the mode, so on a scaled display
 *                        (physical 1920x1200, logical 1500x937) fullscreen and
 *                        maximized windows overshoot the screen. Off by default
 *                        because it changes what every client sees.
 *   WLSHIM_DEBUG         log what the shim does
 *   WLSHIM_BT            print a backtrace on SIGSEGV
 *   WLSHIM_SHM_XRGB      experiment: send ARGB8888 shm buffers as XRGB8888
 *                        (breaks cursors, which need alpha)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

struct wl_proxy;
struct wl_interface;

typedef void *(*marshal_fn)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, ...);
typedef const char *(*class_fn)(struct wl_proxy *);
typedef int (*add_listener_fn)(struct wl_proxy *, void (**)(void), void *);
typedef void (*global_fn)(void *, struct wl_proxy *, uint32_t, const char *, uint32_t);
typedef void (*set_ud_fn)(struct wl_proxy *, void *);
typedef void *(*get_ud_fn)(struct wl_proxy *);
typedef uint32_t (*get_ver_fn)(struct wl_proxy *);
typedef void (*set_queue_fn)(struct wl_proxy *, void *);
typedef int (*dq_fn)(void *, void *);
typedef int (*d_fn)(void *);
typedef void (*mode_fn)(void *, struct wl_proxy *, uint32_t, int32_t, int32_t, int32_t);
typedef void (*proxy_fn)(void *, struct wl_proxy *);
typedef void (*size_fn)(void *, struct wl_proxy *, int32_t, int32_t);

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* number of variadic words forwarded to wl_proxy_marshal_flags; covers every
 * core request (reads past the real arguments are harmless on x86-64) */
#define NWORDS 12

#define MARSHAL_FLAG_DESTROY 1

/* wl_registry */
#define REGISTRY_BIND 0
/* wl_surface */
#define SURFACE_DESTROY 0
#define SURFACE_ATTACH 1
#define SURFACE_COMMIT 6
#define SURFACE_DAMAGE 2
#define SURFACE_DAMAGE_BUFFER 9
/* zwp_linux_dmabuf_v1 */
#define DMABUF_GET_DEFAULT_FEEDBACK 2
#define DMABUF_GET_SURFACE_FEEDBACK 3

#define MAX_FAKE_GLOBALS 16
#define MAX_BOUND_GLOBALS 128
#define MAX_FAKE_PROXIES 64
#define MAX_PACED_SURFACES 64
#define MAX_TRACKED 16 /* registries / outputs / xdg_outputs */
/* zxdg_output_manager_v1 */
#define XDG_OUTPUT_MANAGER_GET_XDG_OUTPUT 1
/* wl_output.mode flags */
#define WL_OUTPUT_MODE_CURRENT 1u

static marshal_fn real_marshal;
static class_fn real_class;
static add_listener_fn real_add_listener;

static int debug;
static int shm_xrgb;
static int logical_mode;
static int64_t cap_ns; /* frame interval in ns, 0 = no cap */

/* one lock for all tables below; none of them are hot enough to matter */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* globals we advertise at a higher version than the compositor offers */
static struct {
  char name[64];
  uint32_t ver;
} fake_globals[MAX_FAKE_GLOBALS];
static int nfake_globals;

/* globals we actually upgraded, with the version the compositor really has */
static struct {
  struct wl_proxy *reg;
  uint32_t name, real;
} upgraded[MAX_BOUND_GLOBALS];
static int nupgraded;

static void segv_handler(int sig) {
  void *bt[40];
  int n = backtrace(bt, 40);
  fprintf(stderr, "wlshim: caught signal %d, backtrace:\n", sig);
  backtrace_symbols_fd(bt, n, 2);
  _exit(139);
}

__attribute__((constructor)) static void wlshim_init(void) {
  debug = getenv("WLSHIM_DEBUG") != NULL;
  shm_xrgb = getenv("WLSHIM_SHM_XRGB") != NULL;

  const char *lm = getenv("WLSHIM_LOGICAL_MODE");
  logical_mode = lm && *lm && strcmp(lm, "0") != 0;

  const char *fps = getenv("WLSHIM_FPS");
  if (fps && atoi(fps) > 0)
    cap_ns = 1000000000LL / atoi(fps);

  if (getenv("WLSHIM_BT"))
    signal(SIGSEGV, segv_handler);

  const char *e = getenv("WLSHIM_FAKE");
  char *s = strdup(e ? e : "wl_compositor=4");
  char *save = NULL;
  for (char *t = strtok_r(s, ",", &save); t && nfake_globals < MAX_FAKE_GLOBALS;
       t = strtok_r(NULL, ",", &save)) {
    char *eq = strchr(t, '=');
    if (!eq)
      continue;
    *eq = 0;
    snprintf(fake_globals[nfake_globals].name, sizeof fake_globals[0].name,
             "%s", t);
    fake_globals[nfake_globals].ver = (uint32_t)atoi(eq + 1);
    nfake_globals++;
  }
  free(s);
}

/* finding the real libwayland-client */

/* libwayland-client is usually loaded privately (e.g. wine dlopen()s
 * winewayland.so), so it isn't in the global scope and dlsym(RTLD_NEXT) can't
 * see it. Fall back to the copy that is already loaded. */
static void *find_real(const char *name) {
  void *p = dlsym(RTLD_NEXT, name);
  if (!p) {
    void *h = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (h)
      p = dlsym(h, name);
  }
  return p;
}

static void resolve(void) {
  if (!real_marshal)
    real_marshal = (marshal_fn)find_real("wl_proxy_marshal_flags");
  if (!real_class)
    real_class = (class_fn)find_real("wl_proxy_get_class");
  if (!real_add_listener)
    real_add_listener = (add_listener_fn)find_real("wl_proxy_add_listener");
  if (!real_marshal || !real_class || !real_add_listener) {
    fprintf(stderr, "wlshim: cannot find libwayland-client symbols\n");
    abort();
  }
}

/* fake proxies
 *
 * Objects that exist only inside this process. They must NOT come from
 * wl_proxy_create(): that consumes a real object ID which Exo never hears
 * about, and Exo then rejects the next request with a protocol error.
 */

static struct {
  void *p;
  void *ud;
  void (**impl)(void);
  void *data;
  int pending; /* listener added, events not yet delivered */
} fakes[MAX_FAKE_PROXIES];

/* lets the common case (no fakes alive) skip the lock and table scan */
static int nfakes_live;

static int fake_idx(const void *p) {
  int r = -1;
  if (!__atomic_load_n(&nfakes_live, __ATOMIC_ACQUIRE))
    return -1;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_FAKE_PROXIES; i++) {
    if (fakes[i].p && fakes[i].p == p) {
      r = i;
      break;
    }
  }
  pthread_mutex_unlock(&lock);
  return r;
}

static void *fake_new(void) {
  void *p = calloc(1, 64);
  pthread_mutex_lock(&lock);
  for (int i = 0; p && i < MAX_FAKE_PROXIES; i++) {
    if (!fakes[i].p) {
      memset(&fakes[i], 0, sizeof fakes[i]);
      fakes[i].p = p;
      __atomic_add_fetch(&nfakes_live, 1, __ATOMIC_RELEASE);
      pthread_mutex_unlock(&lock);
      return p;
    }
  }
  pthread_mutex_unlock(&lock);
  free(p);
  fprintf(stderr, "wlshim: out of fake proxies\n");
  return NULL;
}

static void fake_free(void *p) {
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_FAKE_PROXIES; i++) {
    if (fakes[i].p == p) {
      fakes[i].p = NULL;
      fakes[i].pending = 0;
      __atomic_sub_fetch(&nfakes_live, 1, __ATOMIC_RELEASE);
      break;
    }
  }
  pthread_mutex_unlock(&lock);
  free(p);
}

/* libwayland entry points Mesa may call on a feedback object. Fakes are
 * handled here; everything else is forwarded. */

void wl_proxy_set_user_data(struct wl_proxy *p, void *d) {
  static set_ud_fn real;
  int i = fake_idx(p);
  if (i >= 0) {
    pthread_mutex_lock(&lock);
    fakes[i].ud = d;
    pthread_mutex_unlock(&lock);
    return;
  }
  if (!real)
    real = (set_ud_fn)find_real("wl_proxy_set_user_data");
  real(p, d);
}

void *wl_proxy_get_user_data(struct wl_proxy *p) {
  static get_ud_fn real;
  int i = fake_idx(p);
  if (i >= 0) {
    pthread_mutex_lock(&lock);
    void *d = fakes[i].ud;
    pthread_mutex_unlock(&lock);
    return d;
  }
  if (!real)
    real = (get_ud_fn)find_real("wl_proxy_get_user_data");
  return real(p);
}

uint32_t wl_proxy_get_version(struct wl_proxy *p) {
  static get_ver_fn real;
  if (fake_idx(p) >= 0)
    return 4;
  if (!real)
    real = (get_ver_fn)find_real("wl_proxy_get_version");
  return real(p);
}

void wl_proxy_set_queue(struct wl_proxy *p, void *q) {
  static set_queue_fn real;
  if (fake_idx(p) >= 0)
    return; /* nothing is ever dispatched on a fake */
  if (!real)
    real = (set_queue_fn)find_real("wl_proxy_set_queue");
  real(p, q);
}

/* fake zwp_linux_dmabuf_feedback_v1 (Exo only has dmabuf v3) */

struct wl_array_ {
  size_t size, alloc;
  void *data;
};

/* listener layout, in event order */
struct fb_listener {
  void (*done)(void *, struct wl_proxy *);
  void (*format_table)(void *, struct wl_proxy *, int32_t, uint32_t);
  void (*main_device)(void *, struct wl_proxy *, struct wl_array_ *);
  void (*tranche_done)(void *, struct wl_proxy *);
  void (*tranche_target_device)(void *, struct wl_proxy *, struct wl_array_ *);
  void (*tranche_formats)(void *, struct wl_proxy *, struct wl_array_ *);
  void (*tranche_flags)(void *, struct wl_proxy *, uint32_t);
};

/* Send the events a real compositor would: a format table, the main device,
 * and a single tranche covering every table entry. Only implicit (INVALID) and
 * linear modifiers are offered, so the client never picks a tiled layout. */
static void emit_feedback(struct wl_proxy *fb, struct fb_listener *l,
                          void *data) {
  static const struct {
    uint32_t fmt, pad;
    uint64_t mod;
  } table[] = {
      {0x34325258, 0, 0x00ffffffffffffffULL}, {0x34325258, 0, 0}, /* XRGB8888 */
      {0x34325241, 0, 0x00ffffffffffffffULL}, {0x34325241, 0, 0}, /* ARGB8888 */
      {0x34324258, 0, 0x00ffffffffffffffULL}, {0x34324258, 0, 0}, /* XBGR8888 */
      {0x34324241, 0, 0x00ffffffffffffffULL}, {0x34324241, 0, 0}, /* ABGR8888 */
  };

  const char *node = getenv("WLSHIM_RENDER_NODE");
  struct stat st;
  if (stat(node ? node : "/dev/dri/renderD128", &st)) {
    fprintf(stderr, "wlshim: cannot stat render node\n");
    return;
  }
  dev_t dev = st.st_rdev;
  struct wl_array_ dev_arr = {sizeof dev, sizeof dev, &dev};

  uint16_t idx[ARRAY_LEN(table)];
  for (size_t i = 0; i < ARRAY_LEN(idx); i++)
    idx[i] = (uint16_t)i;
  struct wl_array_ idx_arr = {sizeof idx, sizeof idx, idx};

  int fd = memfd_create("wlshim-fmt", MFD_CLOEXEC);
  if (fd < 0 || write(fd, table, sizeof table) != (ssize_t)sizeof table) {
    fprintf(stderr, "wlshim: format table setup failed\n");
    if (fd >= 0)
      close(fd);
    return;
  }

  if (debug)
    fprintf(stderr, "wlshim: delivering fake dmabuf feedback, device %u:%u\n",
            major(dev), minor(dev));

  if (l->format_table)
    l->format_table(data, fb, fd, sizeof table); /* the client closes fd */
  else
    close(fd);
  if (l->main_device)
    l->main_device(data, fb, &dev_arr);
  if (l->tranche_target_device)
    l->tranche_target_device(data, fb, &dev_arr);
  if (l->tranche_flags)
    l->tranche_flags(data, fb, 0);
  if (l->tranche_formats)
    l->tranche_formats(data, fb, &idx_arr);
  if (l->tranche_done)
    l->tranche_done(data, fb);
  if (l->done)
    l->done(data, fb);
}

/* Deliver events to fake objects whose listener was added since the last
 * dispatch. Real events only arrive during a dispatch or roundtrip, and Mesa
 * finishes initializing its state after add_listener, so delivering any
 * earlier crashes it. Returns how many objects were served. */
static int flush_fakes(void) {
  int n = 0;
  for (;;) {
    void *p = NULL, *data = NULL;
    void (**impl)(void) = NULL;

    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_FAKE_PROXIES; i++) {
      if (fakes[i].p && fakes[i].pending && fakes[i].impl) {
        fakes[i].pending = 0;
        p = fakes[i].p;
        impl = fakes[i].impl;
        data = fakes[i].data;
        break;
      }
    }
    pthread_mutex_unlock(&lock);

    if (!p)
      return n;
    emit_feedback(p, (struct fb_listener *)impl,
                  data); /* p may be freed in here */
    n++;
  }
}

/* Every dispatch/roundtrip entry point flushes pending fake events first. */

int wl_display_roundtrip_queue(void *d, void *q) {
  static dq_fn real;
  flush_fakes();
  if (!real)
    real = (dq_fn)find_real("wl_display_roundtrip_queue");
  return real(d, q);
}

int wl_display_roundtrip(void *d) {
  static d_fn real;
  flush_fakes();
  if (!real)
    real = (d_fn)find_real("wl_display_roundtrip");
  return real(d);
}

/* the blocking variants return right after delivering fake events instead of
 * waiting on the socket */

int wl_display_dispatch_queue(void *d, void *q) {
  static dq_fn real;
  int n = flush_fakes();
  if (n)
    return n;
  if (!real)
    real = (dq_fn)find_real("wl_display_dispatch_queue");
  return real(d, q);
}

int wl_display_dispatch(void *d) {
  static d_fn real;
  int n = flush_fakes();
  if (n)
    return n;
  if (!real)
    real = (d_fn)find_real("wl_display_dispatch");
  return real(d);
}

int wl_display_dispatch_queue_pending(void *d, void *q) {
  static dq_fn real;
  int n = flush_fakes();
  if (!real)
    real = (dq_fn)find_real("wl_display_dispatch_queue_pending");
  int r = real(d, q);
  return r < 0 ? r : r + n;
}

int wl_display_dispatch_pending(void *d) {
  static d_fn real;
  int n = flush_fakes();
  if (!real)
    real = (d_fn)find_real("wl_display_dispatch_pending");
  int r = real(d);
  return r < 0 ? r : r + n;
}

/* listener wrappers
 *
 * Every wrapper is registered with the client's own user-data pointer, so the
 * events we don't touch (and the ones we forward) reach the client's handlers
 * with the data they expect. Our per-object state is found by proxy instead.
 * The contexts are never freed: one small allocation per registry/output.
 */

struct reg_ctx {
  struct wl_proxy *reg;
  void (*impl[2])(void); /* global, global_remove */
  global_fn orig_global;
};

struct out_ctx {
  struct wl_proxy *output;
  void (*impl[6])(void); /* geometry, mode, done, scale, name, description */
  mode_fn orig_mode;
  proxy_fn orig_done;
  void *orig_data;
  uint32_t flags;
  int32_t refresh;
  int32_t raw_w, raw_h; /* mode as the compositor sent it */
  int32_t lw, lh;       /* xdg_output logical size, 0 = not known yet */
  int32_t sent_w, sent_h; /* current mode as last reported to the client */
};

struct xdg_ctx {
  struct wl_proxy *xdg;
  void (*impl[5])(void); /* logical_position, logical_size, done, name, description */
  size_fn orig_size;
  proxy_fn orig_done;
};

static struct reg_ctx *regs[MAX_TRACKED];
static int nregs;
static struct out_ctx *outs[MAX_TRACKED];
static int nouts;
static struct xdg_ctx *xdgs[MAX_TRACKED];
static int nxdgs;
static struct {
  struct wl_proxy *xdg, *output;
} xdg_pairs[MAX_TRACKED]; /* which wl_output an xdg_output describes */
static int nxdg_pairs;

#define DEFINE_FIND(fn, type, arr, cnt, field)                                 \
  static type *fn(struct wl_proxy *p) {                                        \
    type *r = NULL;                                                            \
    pthread_mutex_lock(&lock);                                                 \
    for (int i = 0; i < cnt; i++) {                                            \
      if (arr[i]->field == p) {                                                \
        r = arr[i];                                                            \
        break;                                                                 \
      }                                                                        \
    }                                                                          \
    pthread_mutex_unlock(&lock);                                               \
    return r;                                                                  \
  }
DEFINE_FIND(find_reg, struct reg_ctx, regs, nregs, reg)
DEFINE_FIND(find_out, struct out_ctx, outs, nouts, output)
DEFINE_FIND(find_xdg, struct xdg_ctx, xdgs, nxdgs, xdg)

static struct out_ctx *out_for_xdg(struct wl_proxy *x) {
  struct wl_proxy *op = NULL;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < nxdg_pairs; i++) {
    if (xdg_pairs[i].xdg == x) {
      op = xdg_pairs[i].output;
      break;
    }
  }
  pthread_mutex_unlock(&lock);
  return op ? find_out(op) : NULL;
}

/* wl_registry.global interception */

/* Raise the advertised version of the globals listed in WLSHIM_FAKE and
 * remember the real one, so the bind can be downgraded on the wire. */
static void global_wrap(void *data, struct wl_proxy *reg, uint32_t name, const char *iface, uint32_t version) {
  struct reg_ctx *c = find_reg(reg);

  for (int i = 0; iface && i < nfake_globals; i++) {
    if (!strcmp(iface, fake_globals[i].name) && version < fake_globals[i].ver) {
      pthread_mutex_lock(&lock);
      if (nupgraded < MAX_BOUND_GLOBALS) {
        upgraded[nupgraded].reg = reg;
        upgraded[nupgraded].name = name;
        upgraded[nupgraded].real = version;
        nupgraded++;
      }
      pthread_mutex_unlock(&lock);

      if (debug)
        fprintf(stderr, "wlshim: advertising %s v%u as v%u\n", iface, version,
            fake_globals[i].ver);
      version = fake_globals[i].ver;
      break;
    }
  }

  if (c && c->orig_global)
    c->orig_global(data, reg, name, iface, version);
}

/* logical size as the wl_output mode */

/* If the logical size differs from the mode the client was last told about,
 * send it a fresh "current" mode with the logical dimensions. */
static void apply_logical(struct out_ctx *o) {
  int32_t w = 0, h = 0;

  pthread_mutex_lock(&lock);
  if (o->lw > 0 && o->lh > 0 && (o->lw != o->sent_w || o->lh != o->sent_h)) {
    w = o->lw;
    h = o->lh;
    o->sent_w = w;
    o->sent_h = h;
  }
  pthread_mutex_unlock(&lock);

  if (w && o->orig_mode) {
    if (debug)
      fprintf(stderr, "wlshim: output mode %dx%d -> logical %dx%d\n", o->raw_w,
              o->raw_h, w, h);
    o->orig_mode(o->orig_data, o->output, o->flags | WL_OUTPUT_MODE_CURRENT, w,
                 h, o->refresh);
  }
}

static void out_mode_wrap(void *data, struct wl_proxy *out, uint32_t flags,
                          int32_t w, int32_t h, int32_t refresh) {
  struct out_ctx *o = find_out(out);
  if (!o)
    return;

  int32_t fw = w, fh = h;
  pthread_mutex_lock(&lock);
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    o->flags = flags;
    o->refresh = refresh;
    o->raw_w = w;
    o->raw_h = h;
    if (o->lw > 0 && o->lh > 0) {
      fw = o->lw;
      fh = o->lh;
    }
    o->sent_w = fw;
    o->sent_h = fh;
  }
  pthread_mutex_unlock(&lock);

  if (o->orig_mode)
    o->orig_mode(data, out, flags, fw, fh, refresh);
}

/* xdg_output v3 sends a wl_output.done after its properties, older versions
 * use xdg_output.done: correct the mode before the client sees either. */
static void out_done_wrap(void *data, struct wl_proxy *out) {
  struct out_ctx *o = find_out(out);
  if (o)
    apply_logical(o);
  if (o && o->orig_done)
    o->orig_done(data, out);
}

static void xdg_size_wrap(void *data, struct wl_proxy *x, int32_t w, int32_t h) {
  struct xdg_ctx *c = find_xdg(x);
  struct out_ctx *o = out_for_xdg(x);
  if (o) {
    pthread_mutex_lock(&lock);
    o->lw = w;
    o->lh = h;
    pthread_mutex_unlock(&lock);
  }
  if (c && c->orig_size)
    c->orig_size(data, x, w, h);
}

static void xdg_done_wrap(void *data, struct wl_proxy *x) {
  struct xdg_ctx *c = find_xdg(x);
  struct out_ctx *o = out_for_xdg(x);
  if (o)
    apply_logical(o);
  if (c && c->orig_done)
    c->orig_done(data, x);
}

/* wl_proxy_add_listener */

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**impl)(void), void *data) {
  resolve();

  /* fake feedback object: just remember the listener, flush_fakes() delivers */
  if (impl) {
    int i = fake_idx(proxy);
    if (i >= 0) {
      pthread_mutex_lock(&lock);
      fakes[i].impl = impl;
      fakes[i].data = data;
      fakes[i].pending = 1;
      pthread_mutex_unlock(&lock);
      return 0;
    }
  }

  const char *cls = real_class(proxy);
  if (cls && impl) {
    if (!strcmp(cls, "wl_registry")) {
      struct reg_ctx *c = calloc(1, sizeof *c);
      if (c) {
        c->reg = proxy;
        c->impl[0] = (void (*)(void))global_wrap;
        c->impl[1] = impl[1]; /* global_remove: untouched, client data */
        c->orig_global = (global_fn)impl[0];
        pthread_mutex_lock(&lock);
        int ok = nregs < MAX_TRACKED;
        if (ok)
          regs[nregs++] = c;
        pthread_mutex_unlock(&lock);
        if (ok)
          return real_add_listener(proxy, c->impl, data);
        free(c);
      }
    } else if (logical_mode && !strcmp(cls, "wl_output")) {
      struct out_ctx *o = calloc(1, sizeof *o);
      if (o) {
        o->output = proxy;
        memcpy(o->impl, impl, sizeof o->impl);
        o->impl[1] = (void (*)(void))out_mode_wrap;
        o->impl[2] = (void (*)(void))out_done_wrap;
        o->orig_mode = (mode_fn)impl[1];
        o->orig_done = (proxy_fn)impl[2];
        o->orig_data = data;
        pthread_mutex_lock(&lock);
        int ok = nouts < MAX_TRACKED;
        if (ok)
          outs[nouts++] = o;
        pthread_mutex_unlock(&lock);
        if (ok)
          return real_add_listener(proxy, o->impl, data);
        free(o);
      }
    } else if (logical_mode && !strcmp(cls, "zxdg_output_v1")) {
      struct xdg_ctx *x = calloc(1, sizeof *x);
      if (x) {
        x->xdg = proxy;
        memcpy(x->impl, impl, sizeof x->impl);
        x->impl[1] = (void (*)(void))xdg_size_wrap;
        x->impl[2] = (void (*)(void))xdg_done_wrap;
        x->orig_size = (size_fn)impl[1];
        x->orig_done = (proxy_fn)impl[2];
        pthread_mutex_lock(&lock);
        int ok = nxdgs < MAX_TRACKED;
        if (ok)
          xdgs[nxdgs++] = x;
        pthread_mutex_unlock(&lock);
        if (ok)
          return real_add_listener(proxy, x->impl, data);
        free(x);
      }
    }
  }

  return real_add_listener(proxy, impl, data);
}

/* frame cap */

/* Per-surface pacing state. A frame is a commit that follows an attach of a
 * non-null buffer, so only those commits are delayed. */
static struct {
  void *surface;
  int armed;    /* a non-null buffer was attached since the last commit */
  int64_t next; /* earliest time the next frame may be committed */
} pacers[MAX_PACED_SURFACES];

static int64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* Sleeps until an absolute deadline, so a late frame doesn't push all later
 * ones back. attach's first argument is the buffer pointer (0 = detach). */
static void pace(void *surface, uint32_t op, uintptr_t attach_buffer) {
  int64_t at = 0, now = 0;
  int slot = -1, freeslot = -1;

  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_PACED_SURFACES; i++) {
    if (pacers[i].surface == surface) {
      slot = i;
      break;
    }
    if (!pacers[i].surface && freeslot < 0)
      freeslot = i;
  }
  if (slot < 0 && op != SURFACE_DESTROY && freeslot >= 0) {
    slot = freeslot;
    pacers[slot].surface = surface;
    pacers[slot].armed = 0;
    pacers[slot].next = 0;
  }

  if (slot >= 0) {
    if (op == SURFACE_DESTROY) {
      pacers[slot].surface = NULL;
    } else if (op == SURFACE_ATTACH) {
      pacers[slot].armed = attach_buffer != 0;
    } else if (op == SURFACE_COMMIT && pacers[slot].armed) {
      pacers[slot].armed = 0;
      now = now_ns();
      at = pacers[slot].next;
      if (at < now)
        at = now;
      pacers[slot].next = at + cap_ns;
    }
  }
  pthread_mutex_unlock(&lock);

  if (now && at > now) {
    struct timespec ts = {at / 1000000000LL, at % 1000000000LL};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
  }
}

/* request interception */

void *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *iface, uint32_t version,
    uint32_t flags, ...) {
  uintptr_t a[NWORDS];
  va_list ap;
  va_start(ap, flags);
  for (int i = 0; i < NWORDS; i++)
    a[i] = va_arg(ap, uintptr_t);
  va_end(ap);

  resolve();

  /* fake feedback object: the only request is destroy, swallow it */
  if (fake_idx(proxy) >= 0) {
    if (flags & MARSHAL_FLAG_DESTROY)
      fake_free(proxy);
    return NULL;
  }

  const char *cls = real_class(proxy);
  int is_surface = cls && !strcmp(cls, "wl_surface");

  if (cap_ns && is_surface &&
      (opcode == SURFACE_DESTROY || opcode == SURFACE_ATTACH ||
       opcode == SURFACE_COMMIT))
    pace(proxy, opcode, a[0]);

  /* damage_buffer is v4 only: damage the whole surface instead */
  if (is_surface && opcode == SURFACE_DAMAGE_BUFFER) {
    static int once;
    if (debug && !once++)
      fprintf(stderr, "wlshim: damage_buffer -> damage (whole surface)\n");
    return real_marshal(proxy, SURFACE_DAMAGE, NULL, version, flags, (int32_t)0,
        (int32_t)0, (int32_t)INT32_MAX, (int32_t)INT32_MAX);
  }

  /* dmabuf feedback requests don't exist on Exo: hand back a process-local
   * object, send nothing and consume no object ID */
  if (cls && !strcmp(cls, "zwp_linux_dmabuf_v1") &&
      (opcode == DMABUF_GET_DEFAULT_FEEDBACK ||
       opcode == DMABUF_GET_SURFACE_FEEDBACK))
    return fake_new();

  /* experiment only: wl_shm_pool.create_buffer ARGB8888 -> XRGB8888 */
  if (shm_xrgb && cls && opcode == 0 && !strcmp(cls, "wl_shm_pool") &&
      (uint32_t)a[5] == 0)
    a[5] = 1;

  /* wl_registry.bind: send the real version on the wire, the client proxy
   * keeps the upgraded one */
  if (cls && opcode == REGISTRY_BIND && !strcmp(cls, "wl_registry")) {
    uint32_t gname = (uint32_t)a[0], v = (uint32_t)a[2];
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nupgraded; i++) {
      if (upgraded[i].reg == proxy && upgraded[i].name == gname &&
          v > upgraded[i].real) {
        if (debug)
          fprintf(stderr, "wlshim: bind %s v%u -> v%u on the wire\n",
                  (const char *)a[1], v, upgraded[i].real);
        a[2] = upgraded[i].real;
      }
    }
    pthread_mutex_unlock(&lock);
  }

  void *ret = real_marshal(proxy, opcode, iface, version, flags, a[0], a[1],
      a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);

  /* remember which wl_output each xdg_output describes (args: new_id, output) */
  if (logical_mode && ret && cls && opcode == XDG_OUTPUT_MANAGER_GET_XDG_OUTPUT &&
      !strcmp(cls, "zxdg_output_manager_v1")) {
    pthread_mutex_lock(&lock);
    if (nxdg_pairs < MAX_TRACKED) {
      xdg_pairs[nxdg_pairs].xdg = (struct wl_proxy *)ret;
      xdg_pairs[nxdg_pairs].output = (struct wl_proxy *)a[1];
      nxdg_pairs++;
    }
    pthread_mutex_unlock(&lock);
  }
  return ret;
}
