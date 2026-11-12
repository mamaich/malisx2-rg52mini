/*
 * bctest: do BC1-5 and BC7 textures sample right? For each format the GPU
 * reports as a blit source, random blocks go into a 128x80 image with two
 * mip levels (several 64x64 tiles, a partial one) by vkCmdCopyBufferToImage;
 * vkCmdBlitImage (nearest, same size) turns each level into R8G8B8A8, which
 * the texture unit decodes; both images go back to buffers. The blocks
 * must come back byte for byte, and the texels must match a CPU decode
 * (BC7: mode 6 blocks only) within TOL (default 8: hardware interpolation
 * rounds its own way). Exit status 0 when everything matched.
 *
 * Build (on the PC): clang-19 --target=aarch64-linux-gnu --sysroot=<sysroot>
 *   -I<mesa>/include -O2 bctest.c -o bctest -l:libvulkan.so.1 --ld-path=/usr/bin/ld.lld-19
 * Run on the console: DARKOS_VULKAN=<driver .so> ./bctest
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 128
#define H 80
#define LEVELS 2

#define CHECK(x)                                                                       \
   do {                                                                                \
      VkResult r_ = (x);                                                               \
      if (r_ != VK_SUCCESS) {                                                          \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_);              \
         exit(2);                                                                      \
      }                                                                                \
   } while (0)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties mp;

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type for bits 0x%x flags 0x%x\n", bits, want);
   exit(2);
}

static VkDeviceMemory
alloc_for(VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                              mem_type(req.memoryTypeBits, want)};
   VkDeviceMemory m;
   CHECK(vkAllocateMemory(dev, &ai, NULL, &m));
   return m;
}

static VkBuffer
host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *mem, void **map)
{
   VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
   bi.size = size;
   bi.usage = usage;
   VkBuffer b;
   CHECK(vkCreateBuffer(dev, &bi, NULL, &b));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, b, &req);
   *mem = alloc_for(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   CHECK(vkBindBufferMemory(dev, b, *mem, 0));
   CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
   return b;
}

static VkImage
image(VkFormat f, VkImageUsageFlags usage, VkDeviceMemory *mem)
{
   VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   ci.imageType = VK_IMAGE_TYPE_2D;
   ci.format = f;
   ci.extent = (VkExtent3D){W, H, 1};
   ci.mipLevels = LEVELS;
   ci.arrayLayers = 1;
   ci.samples = VK_SAMPLE_COUNT_1_BIT;
   ci.tiling = VK_IMAGE_TILING_OPTIMAL;
   ci.usage = usage;
   ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   VkImage img;
   CHECK(vkCreateImage(dev, &ci, NULL, &img));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, img, &req);
   *mem = alloc_for(req, 0);
   CHECK(vkBindImageMemory(dev, img, *mem, 0));
   return img;
}

static void
barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
        VkPipelineStageFlags src, VkAccessFlags srca, VkPipelineStageFlags dst, VkAccessFlags dsta)
{
   VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
   b.srcAccessMask = srca;
   b.dstAccessMask = dsta;
   b.oldLayout = from;
   b.newLayout = to;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = img;
   b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1};
   vkCmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- CPU decoders: 16 texels, row-major, RGBA8 ---- */

static void
rgb565(uint16_t c, int o[3])
{
   const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
   o[0] = (r << 3) | (r >> 2);
   o[1] = (g << 2) | (g >> 4);
   o[2] = (b << 3) | (b >> 2);
}

static void
color_block(const uint8_t *p, uint8_t out[16][4], bool four_always)
{
   const uint16_t c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8;
   const uint32_t idx = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
   int e[4][4];
   rgb565(c0, e[0]);
   rgb565(c1, e[1]);
   e[0][3] = e[1][3] = 255;
   for (int k = 0; k < 3; k++) {
      if (c0 > c1 || four_always) {
         e[2][k] = (2 * e[0][k] + e[1][k] + 1) / 3;
         e[3][k] = (e[0][k] + 2 * e[1][k] + 1) / 3;
      } else {
         e[2][k] = (e[0][k] + e[1][k] + 1) / 2;
         e[3][k] = 0;
      }
   }
   e[2][3] = 255;
   e[3][3] = (c0 > c1 || four_always) ? 255 : 0;
   for (int i = 0; i < 16; i++)
      for (int k = 0; k < 4; k++)
         out[i][k] = (uint8_t)e[(idx >> (2 * i)) & 3][k];
}

static void
alpha_block(const uint8_t *p, uint8_t v[16])
{
   int a[8];
   a[0] = p[0];
   a[1] = p[1];
   if (a[0] > a[1]) {
      for (int i = 1; i < 7; i++)
         a[i + 1] = ((7 - i) * a[0] + i * a[1] + 3) / 7;
   } else {
      for (int i = 1; i < 5; i++)
         a[i + 1] = ((5 - i) * a[0] + i * a[1] + 2) / 5;
      a[6] = 0;
      a[7] = 255;
   }
   uint64_t bits = 0;
   for (int i = 0; i < 6; i++)
      bits |= (uint64_t)p[2 + i] << (8 * i);
   for (int i = 0; i < 16; i++)
      v[i] = (uint8_t)a[(bits >> (3 * i)) & 7];
}

static unsigned
bit(const uint8_t *p, unsigned i)
{
   return (p[i >> 3] >> (i & 7)) & 1;
}

static unsigned
bits(const uint8_t *p, unsigned *pos, unsigned n)
{
   unsigned v = 0;
   for (unsigned i = 0; i < n; i++)
      v |= bit(p, (*pos)++) << i;
   return v;
}

/* BC7 mode 6: one subset, RGBA 7.7.7.7 endpoints with a p-bit each, 4-bit
 * indices (the first one 3 bits). */
static void
bc7_mode6(const uint8_t *p, uint8_t out[16][4])
{
   static const int w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
   unsigned pos = 7, e[2][4];
   for (int k = 0; k < 4; k++) {
      e[0][k] = bits(p, &pos, 7);
      e[1][k] = bits(p, &pos, 7);
   }
   const unsigned p0 = bits(p, &pos, 1), p1 = bits(p, &pos, 1);
   for (int k = 0; k < 4; k++) {
      e[0][k] = e[0][k] << 1 | p0;
      e[1][k] = e[1][k] << 1 | p1;
   }
   for (int i = 0; i < 16; i++) {
      const unsigned ix = bits(p, &pos, i ? 4 : 3);
      for (int k = 0; k < 4; k++)
         out[i][k] = (uint8_t)(((64 - w4[ix]) * e[0][k] + w4[ix] * e[1][k] + 32) >> 6);
   }
}

enum kind { K_BC1, K_BC2, K_BC3, K_BC4, K_BC5, K_BC7 };

static const struct {
   VkFormat f;
   const char *name;
   enum kind k;
   unsigned bs;
} formats[] = {
   {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM", K_BC1, 8},
   {VK_FORMAT_BC2_UNORM_BLOCK, "BC2_UNORM", K_BC2, 16},
   {VK_FORMAT_BC3_UNORM_BLOCK, "BC3_UNORM", K_BC3, 16},
   {VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM", K_BC4, 8},
   {VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM", K_BC5, 16},
   {VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM (mode 6)", K_BC7, 16},
};

static void
decode(enum kind k, const uint8_t *p, uint8_t out[16][4])
{
   uint8_t a[16], g[16];
   switch (k) {
   case K_BC1:
      color_block(p, out, false);
      break;
   case K_BC2:
      color_block(p + 8, out, true);
      for (int i = 0; i < 16; i++)
         out[i][3] = (uint8_t)(((p[i / 2] >> (4 * (i & 1))) & 15) * 17);
      break;
   case K_BC3:
      color_block(p + 8, out, true);
      alpha_block(p, a);
      for (int i = 0; i < 16; i++)
         out[i][3] = a[i];
      break;
   case K_BC4:
      alpha_block(p, a);
      for (int i = 0; i < 16; i++) {
         out[i][0] = a[i];
         out[i][1] = out[i][2] = 0;
         out[i][3] = 255;
      }
      break;
   case K_BC5:
      alpha_block(p, a);
      alpha_block(p + 8, g);
      for (int i = 0; i < 16; i++) {
         out[i][0] = a[i];
         out[i][1] = g[i];
         out[i][2] = 0;
         out[i][3] = 255;
      }
      break;
   case K_BC7:
      bc7_mode6(p, out);
      break;
   }
}

static uint32_t rng = 0x12345678;
static uint8_t
rnd(void)
{
   rng ^= rng << 13;
   rng ^= rng >> 17;
   rng ^= rng << 5;
   return (uint8_t)rng;
}

int
main(void)
{
   const int tol = getenv("TOL") ? atoi(getenv("TOL")) : 8;
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "bctest", 1, NULL, 0,
                            VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !n) {
      fprintf(stderr, "no physical device\n");
      return 2;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   printf("device: %s\n", props.deviceName);

   const float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);
   VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

   int failed = 0, tested = 0;
   for (unsigned fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
      const VkFormat f = formats[fi].f;
      const unsigned bs = formats[fi].bs;
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
      const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      printf("%-20s optimal features 0x%08x", formats[fi].name, fp.optimalTilingFeatures);
      if ((fp.optimalTilingFeatures & need) != need) {
         printf(": not offered, skipped\n");
         continue;
      }
      tested++;

      /* Blocks of both levels, back to back. */
      VkDeviceSize off[LEVELS], bytes = 0;
      unsigned bw[LEVELS], bh[LEVELS];
      for (unsigned l = 0; l < LEVELS; l++) {
         bw[l] = ((W >> l) + 3) / 4;
         bh[l] = ((H >> l) + 3) / 4;
         off[l] = bytes;
         bytes += (VkDeviceSize)bw[l] * bh[l] * bs;
      }
      VkDeviceMemory upm, bcm, rgm, bcim, rgim;
      uint8_t *up, *bcback, *rgback;
      VkBuffer upb = host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upm, (void **)&up);
      VkBuffer bcb = host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &bcm, (void **)&bcback);
      VkDeviceSize roff[LEVELS], rbytes = 0;
      for (unsigned l = 0; l < LEVELS; l++) {
         roff[l] = rbytes;
         rbytes += (VkDeviceSize)(W >> l) * (H >> l) * 4;
      }
      VkBuffer rgb = host_buffer(rbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rgm, (void **)&rgback);
      for (VkDeviceSize i = 0; i < bytes; i++)
         up[i] = rnd();
      if (formats[fi].k == K_BC7)
         for (VkDeviceSize i = 0; i < bytes; i += 16)
            up[i] = (up[i] & 0x80) | 0x40; /* mode 6 */
      memset(bcback, 0xcd, bytes);
      memset(rgback, 0xcd, rbytes);

      VkImage bci = image(f, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT, &bcim);
      VkImage rgi = image(VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &rgim);

      VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
      VkCommandBuffer cb;
      CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
      VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CHECK(vkBeginCommandBuffer(cb, &cbi));
      barrier(cb, bci, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT);
      barrier(cb, rgi, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT);
      VkBufferImageCopy cp[LEVELS];
      for (unsigned l = 0; l < LEVELS; l++) {
         cp[l] = (VkBufferImageCopy){off[l], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                                     {0, 0, 0}, {W >> l, H >> l, 1}};
      }
      vkCmdCopyBufferToImage(cb, upb, bci, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, LEVELS, cp);
      barrier(cb, bci, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      for (unsigned l = 0; l < LEVELS; l++) {
         VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                           {{0, 0, 0}, {W >> l, H >> l, 1}},
                           {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                           {{0, 0, 0}, {W >> l, H >> l, 1}}};
         vkCmdBlitImage(cb, bci, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rgi,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
      }
      barrier(cb, rgi, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      vkCmdCopyImageToBuffer(cb, bci, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bcb, LEVELS, cp);
      VkBufferImageCopy rc[LEVELS];
      for (unsigned l = 0; l < LEVELS; l++)
         rc[l] = (VkBufferImageCopy){roff[l], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                                     {0, 0, 0}, {W >> l, H >> l, 1}};
      vkCmdCopyImageToBuffer(cb, rgi, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rgb, LEVELS, rc);
      VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &mb, 0, NULL, 0, NULL);
      CHECK(vkEndCommandBuffer(cb));
      VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VkFence fence;
      CHECK(vkCreateFence(dev, &fci, NULL, &fence));
      VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      CHECK(vkQueueSubmit(q, 1, &si, fence));
      CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));

      const int block_diff = memcmp(up, bcback, bytes);
      int maxd = 0;
      unsigned bad = 0, first_x = 0, first_y = 0, first_l = 0;
      uint8_t got0[4] = {0}, want0[4] = {0};
      for (unsigned l = 0; l < LEVELS; l++) {
         for (unsigned by = 0; by < bh[l]; by++) {
            for (unsigned bx = 0; bx < bw[l]; bx++) {
               uint8_t t[16][4];
               decode(formats[fi].k, up + off[l] + ((VkDeviceSize)by * bw[l] + bx) * bs, t);
               for (unsigned i = 0; i < 16; i++) {
                  const unsigned x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                  if (x >= (W >> l) || y >= (H >> l))
                     continue;
                  const uint8_t *g = rgback + roff[l] + ((VkDeviceSize)y * (W >> l) + x) * 4;
                  int d = 0;
                  for (int k = 0; k < 4; k++) {
                     const int dk = abs((int)g[k] - (int)t[i][k]);
                     d = dk > d ? dk : d;
                  }
                  if (d > maxd)
                     maxd = d;
                  if (d > tol) {
                     if (!bad++) {
                        first_x = x;
                        first_y = y;
                        first_l = l;
                        memcpy(got0, g, 4);
                        memcpy(want0, t[i], 4);
                     }
                  }
               }
            }
         }
      }
      const bool ok = !block_diff && !bad;
      printf(": blocks %s, texels max diff %d, %u over %d%s\n", block_diff ? "DIFFER" : "same", maxd,
             bad, tol, ok ? " - ok" : " - FAILED");
      if (bad)
         printf("   first: level %u (%u,%u) got %u %u %u %u, want %u %u %u %u\n", first_l, first_x,
                first_y, got0[0], got0[1], got0[2], got0[3], want0[0], want0[1], want0[2], want0[3]);
      failed += !ok;

      vkDestroyFence(dev, fence, NULL);
      vkFreeCommandBuffers(dev, pool, 1, &cb);
      vkDestroyImage(dev, bci, NULL);
      vkDestroyImage(dev, rgi, NULL);
      vkDestroyBuffer(dev, upb, NULL);
      vkDestroyBuffer(dev, bcb, NULL);
      vkDestroyBuffer(dev, rgb, NULL);
      vkFreeMemory(dev, upm, NULL);
      vkFreeMemory(dev, bcm, NULL);
      vkFreeMemory(dev, rgm, NULL);
      vkFreeMemory(dev, bcim, NULL);
      vkFreeMemory(dev, rgim, NULL);
   }
   printf("%d formats tested, %d failed\n", tested, failed);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return failed ? 1 : 0;
}
