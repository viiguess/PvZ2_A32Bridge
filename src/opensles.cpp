// OpenSL ES bridge: guest-visible COM-style interfaces (vtables of svc stubs) over the host OpenSL ES objects.
#include "opensles.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <dlfcn.h>
#include <string.h>

#include <mutex>
#include <unordered_map>

#include "runtime/runtime.h"

enum Kind { K_OBJ, K_ENGINE, K_PLAY, K_BQ, K_CONFIG, K_COUNT };

static const int NFN[K_COUNT] = {10, 15, 12, 4, 4};
static u32 g_vtbl[K_COUNT];
static std::mutex g_mu;
static std::unordered_map<u32, std::pair<void*, int>> g_g2h;  // guest obj -> (host itf ptr, kind)
static std::unordered_map<void*, u32> g_h2g;
static SLInterfaceID g_hiid[4];  // host ids: ENGINE, BUFFERQUEUE, PLAY, ANDROIDCONFIGURATION
static u32 g_giid[4];            // guest copies of the 16-byte ids
static const int IID_KIND[4] = {K_ENGINE, K_BQ, K_PLAY, K_CONFIG};

static u32 wrap(void* host, int kind) {
  if (!host)
    return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_h2g.find(host);
  if (it != g_h2g.end())
    return it->second;
  u32 guest_obj = gdata_alloc(4, 4);  // tiny leak per interface object; they are few and long-lived
  *(u32*)H(guest_obj) = g_vtbl[kind];
  g_g2h[guest_obj] = {host, kind};
  g_h2g[host] = guest_obj;
  return guest_obj;
}

static void* host_of(u32 guest_obj) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_g2h.find(guest_obj);
  return it == g_g2h.end() ? nullptr : it->second.first;
}

static SLInterfaceID hiid_of(u32 gid) {
  if (!gid)
    return nullptr;
  for (int i = 0; i < 4; i++)
    if (!memcmp(H(gid), g_hiid[i], 16))
      return g_hiid[i];
  return nullptr;
}

static int kind_of_iid(u32 gid) {
  for (int i = 0; i < 4; i++)
    if (!memcmp(H(gid), g_hiid[i], 16))
      return IID_KIND[i];
  return -1;
}

static std::vector<SLInterfaceID> ids_in(u32 count, u32 guest_obj) {
  std::vector<SLInterfaceID> ids(count);
  for (u32 i = 0; i < count; i++)
    ids[i] = hiid_of(((u32*)H(guest_obj))[i]);
  return ids;
}

// A guest callback registered through an SL interface: the function to call, the user pointer the guest
// gave us, and the guest interface object to pass as the first argument.
struct GuestCallback {
  u32 function;
  u32 user_data;
  u32 self;
};

static void buffer_queue_callback(SLAndroidSimpleBufferQueueItf, void* user_data) {
  GuestCallback* callback = (GuestCallback*)user_data;
  guest_call(callback->function, {callback->self, callback->user_data});
}

static void play_callback(SLPlayItf, void* user_data, SLuint32 event) {
  GuestCallback* callback = (GuestCallback*)user_data;
  guest_call(callback->function, {callback->self, callback->user_data, event});
}

static void object_callback(SLObjectItf, const void* user_data, SLuint32 event, SLresult res, SLuint32 state, void*) {
  GuestCallback* callback = (GuestCallback*)user_data;
  guest_call(callback->function, {callback->self, callback->user_data, event, res, state});
}

static SLresult create_player(SLEngineItf engine, Ctx& ctx, SLObjectItf* out) {
  // guest SLDataSource / SLDataSink: {pLocator, pFormat}
  u32* src = (u32*)ctx.p(2);
  u32* snk = (u32*)ctx.p(3);
  u32* loc = (u32*)H(src[0]);
  static thread_local SLDataLocator_AndroidSimpleBufferQueue queue_locator;
  static thread_local SLDataLocator_OutputMix mix_locator;
  static thread_local SLDataFormat_PCM pcm;
  static thread_local SLDataSource source;
  static thread_local SLDataSink sink;
  if (loc[0] == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE || loc[0] == SL_DATALOCATOR_BUFFERQUEUE) {
    queue_locator.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
    queue_locator.numBuffers = loc[1];
    source.pLocator = &queue_locator;
  } else {
    LOG("unsupported SL source locator %x", loc[0]);
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  if (src[1]) {
    memcpy(&pcm, H(src[1]), sizeof pcm);
    source.pFormat = &pcm;
  } else
    source.pFormat = nullptr;
  u32* sloc = (u32*)H(snk[0]);
  if (sloc[0] == SL_DATALOCATOR_OUTPUTMIX) {
    mix_locator.locatorType = SL_DATALOCATOR_OUTPUTMIX;
    mix_locator.outputMix = (SLObjectItf)host_of(sloc[1]);
    sink.pLocator = &mix_locator;
  } else {
    LOG("unsupported SL sink locator %x", sloc[0]);
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  sink.pFormat = nullptr;
  auto ids = ids_in(ctx.a(4), ctx.a(5));
  return (*engine)->CreateAudioPlayer(engine, out, &source, &sink, ctx.a(4), ids.data(), (const SLboolean*)ctx.p(6));
}

static void sl_call(Ctx& ctx, const Stub& stub) {
  int code = (int)(intptr_t)stub.fn, kind = code / 100, method = code % 100;
  void* host = host_of(ctx.a(0));
  if (!host) {
    LOG("SL call on unknown object (%d/%d)", kind, method);
    ctx.ret(SL_RESULT_PARAMETER_INVALID);
    return;
  }
  SLresult result = SL_RESULT_SUCCESS;
  switch (kind) {
    case K_OBJ: {
      SLObjectItf object = (SLObjectItf)host;
      switch (method) {
        case 0:
          result = (*object)->Realize(object, ctx.a(1));
          break;
        case 1:
          result = (*object)->Resume(object, ctx.a(1));
          break;
        case 2:
          result = (*object)->GetState(object, (SLuint32*)ctx.p(1));
          break;
        case 3: {  // GetInterface(self, iid, void* pInterface)
          int kind = kind_of_iid(ctx.a(1));
          void* itf = nullptr;
          SLInterfaceID id = hiid_of(ctx.a(1));
          if (kind < 0 || !id) {
            result = SL_RESULT_FEATURE_UNSUPPORTED;
            break;
          }
          result = (*object)->GetInterface(object, id, &itf);
          if (result == SL_RESULT_SUCCESS)
            *(u32*)ctx.p(2) = wrap((void*)itf, kind);
          break;
        }
        case 4: {
          auto* callback = new GuestCallback{ctx.a(1), ctx.a(2), ctx.a(0)};
          result = (*object)->RegisterCallback(object, object_callback, callback);
          break;
        }
        case 5:
          (*object)->AbortAsyncOperation(object);
          break;
        case 6: {
          (*object)->Destroy(object);
          std::lock_guard<std::mutex> lock(g_mu);
          g_g2h.erase(ctx.a(0));
          g_h2g.erase(host);
          break;
        }
        case 7:
          result = (*object)->SetPriority(object, ctx.a(1), ctx.a(2));
          break;
        case 8:
          result = (*object)->GetPriority(object, (SLint32*)ctx.p(1), (SLboolean*)ctx.p(2));
          break;
        default:
          result = SL_RESULT_FEATURE_UNSUPPORTED;
      }
      break;
    }
    case K_ENGINE: {
      SLEngineItf engine = (SLEngineItf)host;
      SLObjectItf out = nullptr;
      if (method == 2) {
        result = create_player(engine, ctx, &out);
      } else if (method == 7) {
        auto ids = ids_in(ctx.a(2), ctx.a(3));
        result = (*engine)->CreateOutputMix(engine, &out, ctx.a(2), ids.data(), (const SLboolean*)ctx.p(4));
      } else
        result = SL_RESULT_FEATURE_UNSUPPORTED;
      if (result == SL_RESULT_SUCCESS && out)
        *(u32*)ctx.p(1) = wrap((void*)out, K_OBJ);
      break;
    }
    case K_PLAY: {
      SLPlayItf user_data = (SLPlayItf)host;
      switch (method) {
        case 0:
          result = (*user_data)->SetPlayState(user_data, ctx.a(1));
          break;
        case 1:
          result = (*user_data)->GetPlayState(user_data, (SLuint32*)ctx.p(1));
          break;
        case 2:
          result = (*user_data)->GetDuration(user_data, (SLmillisecond*)ctx.p(1));
          break;
        case 3:
          result = (*user_data)->GetPosition(user_data, (SLmillisecond*)ctx.p(1));
          break;
        case 4: {
          auto* callback = new GuestCallback{ctx.a(1), ctx.a(2), ctx.a(0)};
          result = (*user_data)->RegisterCallback(user_data, play_callback, callback);
          break;
        }
        case 5:
          result = (*user_data)->SetCallbackEventsMask(user_data, ctx.a(1));
          break;
        case 6:
          result = (*user_data)->GetCallbackEventsMask(user_data, (SLuint32*)ctx.p(1));
          break;
        case 7:
          result = (*user_data)->SetMarkerPosition(user_data, ctx.a(1));
          break;
        case 8:
          result = (*user_data)->ClearMarkerPosition(user_data);
          break;
        case 9:
          result = (*user_data)->GetMarkerPosition(user_data, (SLmillisecond*)ctx.p(1));
          break;
        case 10:
          result = (*user_data)->SetPositionUpdatePeriod(user_data, ctx.a(1));
          break;
        case 11:
          result = (*user_data)->GetPositionUpdatePeriod(user_data, (SLmillisecond*)ctx.p(1));
          break;
      }
      break;
    }
    case K_BQ: {
      SLAndroidSimpleBufferQueueItf queue = (SLAndroidSimpleBufferQueueItf)host;
      switch (method) {
        case 0:
          result = (*queue)->Enqueue(queue, ctx.p(1), ctx.a(2));
          break;
        case 1:
          result = (*queue)->Clear(queue);
          break;
        case 2:
          result = (*queue)->GetState(queue, (SLAndroidSimpleBufferQueueState*)ctx.p(1));
          break;
        case 3: {
          auto* callback = new GuestCallback{ctx.a(1), ctx.a(2), ctx.a(0)};
          result = (*queue)->RegisterCallback(queue, buffer_queue_callback, callback);
          break;
        }
      }
      break;
    }
    case K_CONFIG: {
      SLAndroidConfigurationItf config = (SLAndroidConfigurationItf)host;
      if (method == 0)
        result = (*config)->SetConfiguration(config, (const SLchar*)ctx.p(1), ctx.p(2), ctx.a(3));
      else if (method == 1)
        result = (*config)->GetConfiguration(config, (const SLchar*)ctx.p(1), (SLuint32*)ctx.p(2), ctx.p(3));
      else
        result = SL_RESULT_FEATURE_UNSUPPORTED;
      break;
    }
  }
  ctx.ret(result);
}

static void import_slCreateEngine(Ctx& ctx, const Stub&) {
  typedef SLresult (*Fn)(SLObjectItf*, SLuint32, const SLEngineOption*, SLuint32, const SLInterfaceID*,
                         const SLboolean*);
  static Fn fn = (Fn)dlsym(dlopen("libOpenSLES.so", RTLD_NOW), "slCreateEngine");
  SLObjectItf out = nullptr;
  auto ids = ids_in(ctx.a(3), ctx.a(4));
  SLresult result =
      fn(&out, ctx.a(1), (const SLEngineOption*)ctx.p(2), ctx.a(3), ids.data(), (const SLboolean*)ctx.p(5));
  if (result == SL_RESULT_SUCCESS)
    *(u32*)ctx.p(0) = wrap((void*)out, K_OBJ);
  ctx.ret(result);
}

void sles_init() {
  void* lib = dlopen("libOpenSLES.so", RTLD_NOW);
  if (!lib) {
    LOG("libOpenSLES.so missing");
    return;
  }
  const char* names[4] = {"SL_IID_ENGINE", "SL_IID_BUFFERQUEUE", "SL_IID_PLAY", "SL_IID_ANDROIDCONFIGURATION"};
  for (int i = 0; i < 4; i++) {
    g_hiid[i] = *(SLInterfaceID*)dlsym(lib, names[i]);
    g_giid[i] = gdata_alloc(16, 4);
    memcpy(H(g_giid[i]), g_hiid[i], 16);
    u32 cell = gdata_alloc(4, 4);  // the import is a `const SLInterfaceID` variable (a pointer)
    *(u32*)H(cell) = g_giid[i];
    sym_add(names[i], cell);
  }
  {  // import-only IID (device capabilities interface is not provided by Android): give it a unique dummy id so lookups
     // just fail cleanly
    u32 guest_obj = gdata_alloc(16, 4);
    memcpy(H(guest_obj), "A32-AUDIOIODEVCAP", 16);
    u32 cell = gdata_alloc(4, 4);
    *(u32*)H(cell) = guest_obj;
    sym_add("SL_IID_AUDIOIODEVICECAPABILITIES", cell);
  }
  for (int k = 0; k < K_COUNT; k++) {
    g_vtbl[k] = gdata_alloc(4 * NFN[k], 4);
    for (int i = 0; i < NFN[k]; i++)
      ((u32*)H(g_vtbl[k]))[i] = stub_add("SLfn", sl_call, (void*)(intptr_t)(k * 100 + i));
  }
  stub_add("slCreateEngine", import_slCreateEngine);
}
