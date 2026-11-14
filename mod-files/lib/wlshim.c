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
 *   - completes the frame callbacks of cursor surfaces for Xwayland
 *     (WLSHIM_CURSOR_FRAME)
 *   - keeps child processes from inheriting the audio server connection
 *     (WLSHIM_CRAS_CLOEXEC)
 *   - gives a wl_touch listener the shape and orientation handlers it lacks, so a
 *     tap does not abort Xwayland (WLSHIM_TOUCH_STUBS)
 *   - announces XRGB8888 shm buffers as ARGB8888 with opaque alpha, because
 *     Exo swaps their red and blue (WLSHIM_SHM_ARGB)
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
 *   WLSHIM_CURSOR_FRAME  1 = complete the frame callbacks of cursor surfaces
 *                        (those given to wl_pointer.set_cursor) immediately.
 *                        Needed for Xwayland on Exo: Xwayland waits for such a
 *                        callback before sending the next cursor image, Exo
 *                        never sends it, so the cursor freezes on the first one.
 *   WLSHIM_TOUCH_STUBS   1 = add do-nothing handlers for wl_touch.shape and wl_touch.orientation
 *                        (events 5 and 6, wl_seat version 6) to every wl_touch listener
 *                        registered. Needed for Xwayland on Exo: Exo sends them and
 *                        Xwayland 24.1 has no handler, so libwayland aborts it ("listener
 *                        function for opcode 5 of wl_touch is NULL") on the first touch.
 *                        Only for clients that register the five handlers up to
 *                        wl_touch.cancel and nothing after, as Xwayland does
 *   WLSHIM_CRAS_CLOEXEC  default on, 0 = off. Stops child processes from inheriting the
 *                        connection to the ChromeOS audio server (CRAS). libcras opens
 *                        it without close-on-exec, so a Wine process that starts another
 *                        one (a launcher starting a game) hands its connection on. When
 *                        the first process exits, CRAS still sees the connection open
 *                        and keeps that process's stream, which nobody feeds any more;
 *                        as it mixes only as fast as its slowest stream, all audio
 *                        stops until the second process ends.
 *   WLSHIM_SHM_ARGB      default on, 0 = off. Exo shows XRGB8888 shm buffers with red and
 *                        blue swapped. Their create_buffer format is changed to ARGB8888
 *                        and the alpha bytes are set to 0xff at every commit, so they stay
 *                        opaque (buffers that are ARGB8888 to begin with, such as cursors,
 *                        are never touched)
 *   WLSHIM_DEBUG         log what the shim does
 *   WLSHIM_BT            print a backtrace on SIGSEGV
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <stddef.h>
#include <sys/syscall.h>
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
#define SURFACE_FRAME 3
#define SURFACE_DAMAGE_BUFFER 9
/* wl_pointer */
#define POINTER_SET_CURSOR 0
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
static int shm_argb;
static int logical_mode;
static int cursor_frame; /* WLSHIM_CURSOR_FRAME */
static int touch_stubs; /* WLSHIM_TOUCH_STUBS */
static int64_t cap_ns; /* frame interval in ns, 0 = no cap */
static int cras_cloexec; /* WLSHIM_CRAS_CLOEXEC */

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
  { const char *e = getenv("WLSHIM_SHM_ARGB"); shm_argb = !(e && e[0] == '0'); }

  const char *cf = getenv("WLSHIM_CURSOR_FRAME");
  cursor_frame = cf && *cf && strcmp(cf, "0") != 0;

  const char *ts = getenv("WLSHIM_TOUCH_STUBS");
  touch_stubs = ts && *ts && strcmp(ts, "0") != 0;

  const char *lm = getenv("WLSHIM_LOGICAL_MODE");
  logical_mode = lm && *lm && strcmp(lm, "0") != 0;

  const char *cc = getenv("WLSHIM_CRAS_CLOEXEC");
  cras_cloexec = !(cc && !strcmp(cc, "0")); /* on unless explicitly 0 */

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

/* ---- finding the real libwayland-client ---- */

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

/* ---- fake proxies ----
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

/* ---- fake zwp_linux_dmabuf_feedback_v1 (Exo only has dmabuf v3) ---- */

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

/* ---- listener wrappers ----
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

/* ---- wl_registry.global interception ---- */

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

/* ---- logical size as the wl_output mode ---- */

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

/* ---- cursor frame callbacks ----
 *
 * Xwayland throttles cursor updates: it requests a wl_surface.frame callback
 * on its cursor surface and sends no further cursor image until that callback
 * fires. Exo never completes frame callbacks for cursor surfaces, so after the
 * first cursor the pointer image is stuck. With WLSHIM_CURSOR_FRAME set, the
 * frame callback of a surface that was passed to wl_pointer.set_cursor is
 * completed right away, from inside wl_callback_add_listener. */

#define MAX_CURSOR_SURFACES 8
#define MAX_CURSOR_CBS 16

static void *cursor_surfs[MAX_CURSOR_SURFACES];
static int cursor_surf_next;
static void *cursor_cbs[MAX_CURSOR_CBS];
static int cursor_cb_next;
static unsigned cursor_done_count;

static int is_cursor_surface(void *s) {
  int r = 0;
  if (!cursor_frame || !s)
    return 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_CURSOR_SURFACES; i++)
    if (cursor_surfs[i] == s)
      r = 1;
  pthread_mutex_unlock(&lock);
  return r;
}

static void note_cursor_surface(void *s) {
  if (!s)
    return;
  pthread_mutex_lock(&lock);
  int have = 0;
  for (int i = 0; i < MAX_CURSOR_SURFACES; i++)
    if (cursor_surfs[i] == s)
      have = 1;
  if (!have) {
    cursor_surfs[cursor_surf_next] = s;
    cursor_surf_next = (cursor_surf_next + 1) % MAX_CURSOR_SURFACES;
    if (debug)
      fprintf(stderr, "wlshim: cursor surface %p\n", s);
  }
  pthread_mutex_unlock(&lock);
}

static void forget_cursor_surface(void *s) {
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_CURSOR_SURFACES; i++)
    if (cursor_surfs[i] == s)
      cursor_surfs[i] = NULL;
  pthread_mutex_unlock(&lock);
}

static void note_cursor_cb(void *cb) {
  pthread_mutex_lock(&lock);
  cursor_cbs[cursor_cb_next] = cb;
  cursor_cb_next = (cursor_cb_next + 1) % MAX_CURSOR_CBS;
  pthread_mutex_unlock(&lock);
}

static int take_cursor_cb(void *cb) {
  int r = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_CURSOR_CBS; i++) {
    if (cursor_cbs[i] == cb) {
      cursor_cbs[i] = NULL;
      r = 1;
    }
  }
  pthread_mutex_unlock(&lock);
  return r;
}

typedef void (*cb_done_fn)(void *, struct wl_proxy *, uint32_t);
static int64_t now_ns(void);

/* ---- wl_proxy_add_listener ---- */

static void touch_ignore(void) {}

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

  /* callback of a cursor surface: register the listener, then fire done now.
   * The client destroys the callback in its handler, so the proxy must not be
   * touched afterwards; a late done from the compositor is dropped by libwayland */
  if (cursor_frame && cls && impl && !strcmp(cls, "wl_callback") &&
      take_cursor_cb(proxy)) {
    int r = real_add_listener(proxy, impl, data);
    cursor_done_count++;
    if (debug && cursor_done_count <= 3)
      fprintf(stderr, "wlshim: completed a cursor frame callback locally\n");
    if (r == 0 && impl[0])
      ((cb_done_fn)impl[0])(data, proxy, (uint32_t)(now_ns() / 1000000));
    return r;
  }

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
    } else if (touch_stubs && !strcmp(cls, "wl_touch")) {
      /* wl_touch has seven events; the first five (down, up, motion, frame, cancel) come
       * from the client, shape and orientation are ignored. The table lives as long as
       * the process: libwayland keeps pointing at it */
      void (**t)(void) = calloc(7, sizeof *t);
      if (t) {
        memcpy(t, impl, 5 * sizeof *t);
        t[5] = t[6] = touch_ignore;
        return real_add_listener(proxy, t, data);
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

/* ---- frame cap ---- */

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

/* ---- XRGB8888 shm buffers sent as ARGB8888 (see WLSHIM_SHM_ARGB) ---- */

#define SHM_MAX 512

static struct shm_pool { void *proxy; int fd; size_t size; uint8_t *base; int refs; } shm_pools[SHM_MAX];
static struct shm_buf { void *proxy; int pool; uint32_t off, w, h, stride; } shm_bufs[SHM_MAX];
static struct shm_surf { void *proxy; void *buf; } shm_surfs[SHM_MAX];

/* all under `lock` */
static int shm_pool_find(void *p) {
  for (int i = 0; i < SHM_MAX; i++)
    if (shm_pools[i].proxy == p) return i;
  return -1;
}
static void shm_pool_unref(int i) {
  if (--shm_pools[i].refs > 0) return;
  if (shm_pools[i].base) munmap(shm_pools[i].base, shm_pools[i].size);
  if (shm_pools[i].fd >= 0) close(shm_pools[i].fd);
  memset(&shm_pools[i], 0, sizeof shm_pools[i]);
}
static void shm_pool_new(void *proxy, int fd, size_t size) {
  int dfd = fd >= 0 ? dup(fd) : -1;
  if (dfd < 0) return;
  uint8_t *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
  pthread_mutex_lock(&lock);
  for (int i = 0; base != MAP_FAILED && i < SHM_MAX; i++) {
    if (shm_pools[i].refs == 0) {  /* a destroyed pool stays in use while buffers use it */
      shm_pools[i] = (struct shm_pool){proxy, dfd, size, base, 1};
      pthread_mutex_unlock(&lock);
      return;
    }
  }
  pthread_mutex_unlock(&lock);
  if (base != MAP_FAILED) munmap(base, size);
  close(dfd);
}
static void shm_pool_destroy(void *proxy) {
  pthread_mutex_lock(&lock);
  int i = shm_pool_find(proxy);
  if (i >= 0) { shm_pools[i].proxy = NULL; shm_pool_unref(i); }
  pthread_mutex_unlock(&lock);
}
static void shm_pool_resize(void *proxy, size_t size) {
  pthread_mutex_lock(&lock);
  int i = shm_pool_find(proxy);
  if (i >= 0 && size > shm_pools[i].size) {
    void *nb = mremap(shm_pools[i].base, shm_pools[i].size, size, MREMAP_MAYMOVE);
    if (nb != MAP_FAILED) { shm_pools[i].base = nb; shm_pools[i].size = size; }
  }
  pthread_mutex_unlock(&lock);
}
/* can this pool's buffers be converted? */
static int shm_pool_ok(void *proxy) {
  pthread_mutex_lock(&lock);
  int i = shm_pool_find(proxy), ok = i >= 0;
  if (ok) {
    ok = 0;
    for (int j = 0; j < SHM_MAX; j++) if (!shm_bufs[j].proxy) { ok = 1; break; }
  }
  pthread_mutex_unlock(&lock);
  return ok;
}
static void shm_buf_new(void *proxy, void *pool, uint32_t off, uint32_t w, uint32_t h, uint32_t stride) {
  pthread_mutex_lock(&lock);
  int pi = shm_pool_find(pool);
  for (int j = 0; pi >= 0 && j < SHM_MAX; j++) {
    if (!shm_bufs[j].proxy) {
      shm_bufs[j] = (struct shm_buf){proxy, pi, off, w, h, stride};
      shm_pools[pi].refs++;
      break;
    }
  }
  pthread_mutex_unlock(&lock);
}
static void shm_buf_destroy(void *proxy) {
  pthread_mutex_lock(&lock);
  for (int j = 0; j < SHM_MAX; j++) {
    if (shm_bufs[j].proxy == proxy) {
      shm_bufs[j].proxy = NULL;
      shm_pool_unref(shm_bufs[j].pool);
      break;
    }
  }
  for (int k = 0; k < SHM_MAX; k++) if (shm_surfs[k].buf == proxy) shm_surfs[k].buf = NULL;
  pthread_mutex_unlock(&lock);
}
static void shm_surf_attach(void *surf, void *buf) {
  pthread_mutex_lock(&lock);
  int free_i = -1, i;
  for (i = 0; i < SHM_MAX; i++) {
    if (shm_surfs[i].proxy == surf) break;
    if (free_i < 0 && !shm_surfs[i].proxy) free_i = i;
  }
  if (i == SHM_MAX) i = free_i;
  if (i >= 0) { shm_surfs[i].proxy = surf; shm_surfs[i].buf = buf; }
  pthread_mutex_unlock(&lock);
}
static void shm_surf_destroy(void *surf) {
  pthread_mutex_lock(&lock);
  for (int i = 0; i < SHM_MAX; i++)
    if (shm_surfs[i].proxy == surf) memset(&shm_surfs[i], 0, sizeof shm_surfs[i]);
  pthread_mutex_unlock(&lock);
}
/* set the alpha byte of every pixel of the surface's converted buffer */
static void shm_surf_opaque(void *surf) {
  int found = 0, attached = 0;
  unsigned long fixed = 0;
  uint32_t bw = 0, bh = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < SHM_MAX; i++) {
    if (shm_surfs[i].proxy != surf) continue;
    attached = shm_surfs[i].buf != NULL;
    for (int j = 0; shm_surfs[i].buf && j < SHM_MAX; j++) {
      struct shm_buf *b = &shm_bufs[j];
      if (b->proxy != shm_surfs[i].buf) continue;
      struct shm_pool *p = &shm_pools[b->pool];
      if ((uint64_t)b->off + (uint64_t)b->stride * b->h > p->size || b->stride < b->w * 4) break;
      found = 1; bw = b->w; bh = b->h;
      for (uint32_t y = 0; y < b->h; y++) {
        uint32_t *row = (uint32_t *)(p->base + b->off + (size_t)y * b->stride);
        for (uint32_t x = 0; x < b->w; x++) {
          if (debug && (row[x] >> 24) != 0xff) fixed++;
          row[x] |= 0xff000000u;
        }
      }
      break;
    }
    break;
  }
  pthread_mutex_unlock(&lock);
  if (debug) {
    static int n;
    if (found && n < 300) { n++; fprintf(stderr, "wlshim: commit surface %p: %ux%u, %lu pixels made opaque\n", surf, bw, bh, fixed); }
    else if (attached && !found && n < 300) { n++; fprintf(stderr, "wlshim: commit surface %p: buffer attached but not converted\n", surf); }
  }
}

/* ---- request interception ---- */

/* wl_registry.bind of a global we advertised higher: send the real version on the wire, the
 * client proxy keeps the upgraded one. Every way of sending a bind goes through here (see the
 * wl_proxy_marshal_* functions below) */
static void downgrade_bind(struct wl_proxy *proxy, uint32_t gname, const char *iname, uint32_t *v) {
  const char *cls;

  if (fake_idx(proxy) >= 0 || !(cls = real_class(proxy)) || strcmp(cls, "wl_registry"))
    return;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < nupgraded; i++) {
    if (upgraded[i].reg == proxy && upgraded[i].name == gname && *v > upgraded[i].real) {
      if (debug)
        fprintf(stderr, "wlshim: bind %s v%u -> v%u on the wire\n", iname, *v, upgraded[i].real);
      *v = upgraded[i].real;
    }
  }
  pthread_mutex_unlock(&lock);
}


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

  /* cursor surfaces: remember them, and don't pace their (cheap) commits */
  int cursor_surf = 0;
  if (cursor_frame) {
    if (cls && opcode == POINTER_SET_CURSOR && !strcmp(cls, "wl_pointer"))
      note_cursor_surface((void *)a[1]);
    if (is_surface) {
      cursor_surf = is_cursor_surface(proxy);
      if (opcode == SURFACE_DESTROY)
        forget_cursor_surface(proxy);
    }
  }

  if (cap_ns && is_surface && !cursor_surf &&
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

  /* WLSHIM_SHM_ARGB: announce XRGB8888 shm buffers as ARGB8888 (made opaque at commit) */
  int shm_conv = 0, shm_cls = 0; /* shm_cls: 1 wl_shm, 2 wl_shm_pool, 3 wl_buffer */
  if (shm_argb && cls) {
    if (!strcmp(cls, "wl_shm")) shm_cls = 1;
    else if (!strcmp(cls, "wl_shm_pool")) shm_cls = 2;
    else if (!strcmp(cls, "wl_buffer")) shm_cls = 3;
    if (debug && shm_cls == 2 && opcode == 0)
      fprintf(stderr, "wlshim: shm create_buffer %ux%u stride %u format %u\n",
              (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4], (uint32_t)a[5]);
    if (shm_cls == 2 && opcode == 0 && (uint32_t)a[5] == 1 && shm_pool_ok(proxy)) {
      a[5] = 0;
      shm_conv = 1;
    }
    if (shm_cls == 2 && opcode == 1) shm_pool_destroy(proxy);
    if (shm_cls == 2 && opcode == 2) shm_pool_resize(proxy, (uint32_t)a[0]);
    if (shm_cls == 3 && opcode == 0) shm_buf_destroy(proxy);
    if (is_surface && opcode == SURFACE_ATTACH) {
      if (debug && a[0]) fprintf(stderr, "wlshim: attach surface %p buffer %p\n", proxy, (void *)a[0]);
      shm_surf_attach(proxy, (void *)a[0]);
    }
    if (is_surface && opcode == SURFACE_COMMIT) shm_surf_opaque(proxy);
    if (is_surface && opcode == SURFACE_DESTROY) shm_surf_destroy(proxy);
  }

  if (opcode == REGISTRY_BIND) {
    uint32_t v = (uint32_t)a[2];
    downgrade_bind(proxy, (uint32_t)a[0], (const char *)a[1], &v);
    a[2] = v;
  }

  void *ret = real_marshal(proxy, opcode, iface, version, flags, a[0], a[1],
      a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);

  if (shm_cls == 1 && opcode == 0 && ret)
    shm_pool_new(ret, (int)a[1], (uint32_t)a[2]);
  if (shm_conv && ret)
    shm_buf_new(ret, proxy, (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]);

  /* the frame callback of a cursor surface is completed in add_listener */
  if (cursor_surf && ret && opcode == SURFACE_FRAME)
    note_cursor_cb(ret);

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

/* Clients built against other libwayland-client headers send registry binds through
 * wl_proxy_marshal_array_flags or wl_proxy_marshal_constructor_versioned instead of
 * wl_proxy_marshal_flags (Qt does); they get the same downgrade. */
union wl_arg {
  int32_t i;
  uint32_t u;
  const char *s;
  void *p;
};

void *wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *iface, uint32_t version, uint32_t flags,
    union wl_arg *args) {
  typedef void *(*fn)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, union wl_arg *);
  static fn real;

  resolve();
  if (!real)
    real = (fn)find_real("wl_proxy_marshal_array_flags");
  if (!real)
    abort();
  if (args && opcode == REGISTRY_BIND)
    downgrade_bind(proxy, args[0].u, args[1].s, &args[2].u);
  return real(proxy, opcode, iface, version, flags, args);
}

void *wl_proxy_marshal_constructor_versioned(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *iface, uint32_t version, ...) {
  typedef void *(*fn)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, ...);
  static fn real;
  uintptr_t a[NWORDS];
  va_list ap;

  va_start(ap, version);
  for (int i = 0; i < NWORDS; i++)
    a[i] = va_arg(ap, uintptr_t);
  va_end(ap);

  resolve();
  if (!real)
    real = (fn)find_real("wl_proxy_marshal_constructor_versioned");
  if (!real)
    abort();
  if (opcode == REGISTRY_BIND) {
    uint32_t v = (uint32_t)a[2];
    downgrade_bind(proxy, (uint32_t)a[0], (const char *)a[1], &v);
    a[2] = v;
  }
  return real(proxy, opcode, iface, version, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
      a[8], a[9], a[10], a[11]);
}

/* ---- CRAS: keep its connection out of child processes (WLSHIM_CRAS_CLOEXEC) ---- */
int connect(int fd, const struct sockaddr *addr, socklen_t len) {
  static int (*real)(int, const struct sockaddr *, socklen_t);
  if (!real)
    real = dlsym(RTLD_NEXT, "connect");
  if (!real)
    return -1;
  int r = real(fd, addr, len);
  if (r == 0 && cras_cloexec && addr && addr->sa_family == AF_UNIX &&
      len > offsetof(struct sockaddr_un, sun_path)) {
    char path[108];
    size_t n = len - offsetof(struct sockaddr_un, sun_path);
    if (n > sizeof path - 1)
      n = sizeof path - 1;
    memcpy(path, ((const struct sockaddr_un *)addr)->sun_path, n);
    path[n] = 0;
    if (path[0] && strstr(path, "cras")) {
      int fl = fcntl(fd, F_GETFD);
      if (fl >= 0 && !(fl & FD_CLOEXEC))
        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
      if (debug)
        fprintf(stderr, "wlshim: connection to %s set close-on-exec (fd %d)\n", path, fd);
    }
  }
  return r;
}
