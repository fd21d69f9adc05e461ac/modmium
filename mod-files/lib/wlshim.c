// LD_PRELOAD shim for wayland clients on v4 to be translated to Exo's v3
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wl_proxy;
struct wl_interface;

typedef void *(*marshal_fn)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, ...);
typedef const char *(*class_fn)(struct wl_proxy *);
typedef int (*add_listener_fn)(struct wl_proxy *, void (**)(void), void*);
typedef void (*global_fn)(void *, struct wl_proxy *, uint32_t, const char *, uint32_t);

#define NWORDS 12 /* variadic words forwarded, covers core requests */
#define REGISTRY_BIND 0
#define SURFACE_DAMAGE 2
#define SURFACE_DAMAGE_BUFFER 9
#define MAXF 16
#define MAXG 128

static marshal_fn real_marshal;
static class_fn real_class;
static add_listener_fn real_add_listener;
static int debug;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct { char name[64]; uint32_t ver; } fakes[MAXF];
static int nfakes;
static struct { struct wl_proxy *reg; uint32_t name, real; } globals[MAXG];
static int nglobals;

__attribute__((constructor)) static void wlshim_init(void) {
  debug = getenv("WLSHIM_DEBUG") != NULL;
  const char *e = getenv("WLSHIM_FAKE");
  char *s = strdup(e ? e : "wl_compositor=4");
  char *save = NULL;
  for (char *t = strtok_r(s, ",", &save); t && nfakes < MAXF;
      t = strtok_r(NULL, ",", &save)) {
    char *eq = strchr(t, '=');
    if (!eq) continue;
    *eq = 0;
    snprintf(fakes[nfakes].name, sizeof fakes[0].name, "%s", t);
    fakes[nfakes].ver = (uint32_t)atoi(eq + 1);
    nfakes++;
  }
  free(s);
}

/* libwayland-client usually gets loaded private (most apparently in wine dlopen()'ing winewayland.so) so it's not in the global scope.
 * thus, dlsym(RTLD_NEXT) doesn't see it and we need to fall back to the already loaded lib */
static void *find_real(const char *name) {
  void *p = dlsym(RTLD_NEXT, name);
  if (!p) {
    void *h = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (h)
      p = dlsym(h, name);
  }
  return p;
}

// make sure above didn't mess up
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



// now to intercept wl_registry.global

struct reg_ctx {
  void (*impl[2])(void); // global, global_remove
  global_fn orig_global;
  void *orig_data;
};

static void global_wrap(void *data, struct wl_proxy *reg, uint32_t name, const char *iface, uint32_t version) {
  struct reg_ctx *c = data;
  for (int i = 0; iface && i < nfakes; i++) {
    if (!strcmp(iface, fakes[i].name) && version < fakes[i].ver) {
      pthread_mutex_lock(&lock);
      if (nglobals < MAXG) {
        globals[nglobals].reg = reg;
        globals[nglobals].name = name;
        globals[nglobals].real = version;
        nglobals++;
      }
      pthread_mutex_unlock(&lock);
      if (debug)
        fprintf(stderr, "wlshim: advertising %s v%u as v%u\n",
            iface, version, fakes[i].ver);
      version = fakes[i].ver;
      break;
    }
  }
  if (c->orig_global)
    c->orig_global(c->orig_data, reg, name, iface, version);
}

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**impl)(void), void *data) {
  resolve();
  const char *cls = real_class ? real_class(proxy) : NULL;
  if (cls && !strcmp(cls, "wl_registry") && impl) {
    struct reg_ctx *c = calloc(1, sizeof *c);
    if (c) {
      c->impl[0] = (void (*)(void))global_wrap;
      c->impl[1] = impl[1];
      c->orig_global = (global_fn)impl[0];
      c->orig_data = data;
      return real_add_listener(proxy, c->impl, c);
    }
  }
  return real_add_listener(proxy, impl, data);
}



// and to intercept protocol requests

void *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *iface, uint32_t version, uint32_t flags, ...) {
  uintptr_t a[NWORDS];
  va_list ap;
  va_start(ap, flags);
  for (int i = 0; i < NWORDS; i++)
    a[i] = va_arg(ap, uintptr_t);
  va_end(ap);

  resolve();
  const char *cls = real_class ? real_class(proxy) : NULL;

  if (cls && opcode == SURFACE_DAMAGE_BUFFER && !strcmp(cls, "wl_surface")) {
    static int once;
    if (debug && !once++)
      fprintf(stderr, "wlshim: damage_buffer -> damage (whole surface)\n");
    return real_marshal(proxy, SURFACE_DAMAGE, NULL, version, flags, (int32_t)0, (int32_t)0, (int32_t)INT32_MAX, (int32_t)INT32_MAX);
  }

  if (cls && opcode == REGISTRY_BIND && !strcmp(cls, "wl_registry")) {
    uint32_t gname = (uint32_t)a[0], v = (uint32_t)a[2];
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nglobals; i++) {
      if (globals[i].reg == proxy && globals[i].name == gname && v > globals[i].real) {
        if (debug)
          fprintf(stderr, "wlshim: bind %s v%u -> v%u on the wire\n",
              (const char *)a[1], v, globals[i].real);
        a[2] = globals[i].real; // wire only, client proxy keeps "version"
      }
    }
    pthread_mutex_unlock(&lock);
  }

  return real_marshal(proxy, opcode, iface, version, flags, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}
