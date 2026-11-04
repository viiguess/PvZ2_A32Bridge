#include "live_patch.h"

#ifdef A32_FREE_SHOP
#include "free_shop.h"
#endif

int apply_live_patches(const Image& image) {
  int applied = 0;
#ifdef A32_FREE_SHOP
  free_shop::Result r = free_shop::apply((uint8_t*)H(image.base), image.size);
  if (r.applied) {
    applied++;
    LOG("live patch free_shop: applied (tail %08x, paymentComplete %08x, service %08x, dialog %08x, success %08x)",
        image.base + r.tail, image.base + r.payment_complete, image.base + r.service_check, image.base + r.dialog_check,
        image.base + r.success_dialog);
  } else {
    LOG("live patch free_shop: not applied (%s)", r.error);
  }
#else
  (void)image;
#endif
  return applied;
}
