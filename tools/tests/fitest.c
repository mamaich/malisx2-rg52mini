/*
 * fitest: do instanced draws read per-instance attributes from firstInstance?
 * (upstream malisx2 daf00ab: on v7 the attribute unit also moves a divided
 * buffer's element by the draw's Offset start, so vkCmdDraw(6, 2, 24, 10)
 * drew instances 34 and 35.)
 *
 * A 16x16 R8G8B8A8 target. Binding 0 (per vertex): 64 vec2 positions, a
 * full-target quad at vertices 0..5 and 24..29, zeros (empty triangles)
 * elsewhere. Binding 1 (per instance): 64 floats, element i = i. The vertex
 * shader puts even gl_InstanceIndex on the left half and odd on the right
 * and passes the per-instance float on; the fragment shader writes it /255
 * to red. Each case clears, draws, reads back the left and right pixels and
 * compares them with the instances the draw should have read.
 *
 * Build (on the PC): clang-19 --target=aarch64-linux-gnu --sysroot=<sysroot>
 *   -I<mesa>/include -O2 fitest.c -o fitest -l:libvulkan.so.1 --ld-path=/usr/bin/ld.lld-19
 * Shaders (on the console): glslangValidator -V fi.vert -o fi.vert.spv, the same for fi.frag.
 * Run: DARKOS_VULKAN=<driver .so> ./fitest <dir with fi.vert.spv, fi.frag.spv> [indirect]
 * "indirect" adds the same draws through vkCmdDrawIndirect and
 * vkCmdDrawIndexedIndirect (one command a call, and two in one call), when
 * the device has multiDrawIndirect and drawIndirectFirstInstance: drivers
 * before malisx2 0.4.0 crash on them, so they are not run by default.
 * Exit status 0 when every case matched.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 16
#define H 16
#define NV 64

#define CHECK(x)                                                                       \
   do {                                                                                \
      VkResult r_ = (x);                                                               \
      if (r_ != VK_SUCCESS) {                                                          \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_);              \
         exit(2);                                                                      \
      }                                                                                \
   } while (0)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type for 0x%x\n", want);
   exit(2);
}

struct buf {
   VkBuffer b;
   VkDeviceMemory m;
   void *map;
};

static struct buf
make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *data)
{
   struct buf r;
   VkBufferCreateInfo bi = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
   };
   CHECK(vkCreateBuffer(dev, &bi, NULL, &r.b));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, r.b, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
      .memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   CHECK(vkAllocateMemory(dev, &ai, NULL, &r.m));
   CHECK(vkBindBufferMemory(dev, r.b, r.m, 0));
   CHECK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.map));
   if (data)
      memcpy(r.map, data, size);
   else
      memset(r.map, 0, size);
   return r;
}

static VkShaderModule
load_spv(const char *dir, const char *name)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) {
      perror(path);
      exit(2);
   }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(n);
   if (fread(code, 1, n, f) != (size_t)n) {
      perror(path);
      exit(2);
   }
   fclose(f);
   VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code,
   };
   VkShaderModule m;
   CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
   free(code);
   return m;
}

struct draw_case {
   const char *name;
   bool indexed;
   uint32_t count, instances, first, first_instance;
   int32_t vertex_offset;
   int expect_left, expect_right; /* -1: background (0) */
   /* 0: a direct draw; 1: the same command by an indirect call; 2: it and
    * the next case's command in one indirect call (the next case is
    * skipped). */
   int indirect;
};

int
main(int argc, char **argv)
{
   const char *dir = argc > 1 ? argv[1] : ".";
   const bool want_indirect = argc > 2 && !strcmp(argv[2], "indirect");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
   };
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t npd = 1;
   VkPhysicalDevice pd;
   VkResult er = vkEnumeratePhysicalDevices(inst, &npd, &pd);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || npd == 0) {
      fprintf(stderr, "no physical device\n");
      return 2;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("device: %s, driverVersion 0x%x\n", props.deviceName, props.driverVersion);
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
      .queueCount = 1, .pQueuePriorities = &prio,
   };
   VkPhysicalDeviceFeatures have;
   vkGetPhysicalDeviceFeatures(pd, &have);
   const bool indirect = want_indirect && have.multiDrawIndirect && have.drawIndirectFirstInstance;
   if (want_indirect && !indirect)
      puts("no multiDrawIndirect / drawIndirectFirstInstance: indirect cases skipped");
   VkPhysicalDeviceFeatures feat = {
      .multiDrawIndirect = indirect,
      .drawIndirectFirstInstance = indirect,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci, .pEnabledFeatures = &feat,
   };
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   /* Target image. */
   VkImageCreateInfo imci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1,
      .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage img;
   CHECK(vkCreateImage(dev, &imci, NULL, &img));
   VkMemoryRequirements ireq;
   vkGetImageMemoryRequirements(dev, img, &ireq);
   VkMemoryAllocateInfo iai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = ireq.size,
      .memoryTypeIndex = mem_type(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory imem;
   CHECK(vkAllocateMemory(dev, &iai, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));
   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView view;
   CHECK(vkCreateImageView(dev, &vci, NULL, &view));

   /* Render pass: clear, store, then transfer source. */
   VkAttachmentDescription att = {
      .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   };
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
      .pColorAttachments = &cref,
   };
   VkSubpassDependency dep = {
      .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   VkRenderPassCreateInfo rpci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
      .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1,
      .pDependencies = &dep,
   };
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
   VkFramebufferCreateInfo fbci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
      .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1,
   };
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

   /* Pipeline. */
   VkShaderModule vs = load_spv(dir, "fi.vert.spv"), fs = load_spv(dir, "fi.frag.spv");
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
   };
   VkVertexInputBindingDescription vb[2] = {
      {0, 8, VK_VERTEX_INPUT_RATE_VERTEX},
      {1, 4, VK_VERTEX_INPUT_RATE_INSTANCE},
   };
   VkVertexInputAttributeDescription va[2] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
      {1, 1, VK_FORMAT_R32_SFLOAT, 0},
   };
   VkPipelineVertexInputStateCreateInfo vis = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = vb,
      .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va,
   };
   VkPipelineInputAssemblyStateCreateInfo ias = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport vp = {0, 0, W, H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
      .pViewports = &vp, .scissorCount = 1, .pScissors = &sc,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba,
   };
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
      .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
      .pColorBlendState = &cbs, .layout = pl, .renderPass = rp,
   };
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

   /* Buffers. */
   float pos[NV][2];
   memset(pos, 0, sizeof(pos));
   static const float quad[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
   memcpy(pos[0], quad, sizeof(quad));
   memcpy(pos[24], quad, sizeof(quad));
   float tags[NV];
   for (int i = 0; i < NV; i++)
      tags[i] = (float)i;
   uint16_t idx[6] = {4, 5, 6, 7, 8, 9}; /* + vertexOffset 20 = vertices 24..29 */
   struct buf vbuf = make_buffer(sizeof(pos), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, pos);
   struct buf tbuf = make_buffer(sizeof(tags), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, tags);
   struct buf ibuf = make_buffer(sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, idx);
   struct buf rb = make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL);

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0,
   };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(vkCreateFence(dev, &fci, NULL, &fence));

   /* Even instances on the left, odd on the right; a later instance covers
    * an earlier one on the same half. */
   static const struct draw_case cases[] = {
      {"draw(6,2,first 0,fi 10)", false, 6, 2, 0, 10, 0, 10, 11},
      {"draw(6,2,first 24,fi 10)", false, 6, 2, 24, 10, 0, 10, 11},
      {"draw(6,3,first 24,fi 10)", false, 6, 3, 24, 10, 0, 12, 11},
      {"draw(6,1,first 24,fi 12)", false, 6, 1, 24, 12, 0, 12, -1},
      {"draw(6,1,first 24,fi 13)", false, 6, 1, 24, 13, 0, -1, 13},
      {"indexed(6,2,idx 4..9,vo 20,fi 10)", true, 6, 2, 0, 10, 20, 10, 11},
      {"indexed(6,5,idx 4..9,vo 20,fi 7)", true, 6, 5, 0, 7, 20, 10, 11},
      {"ind draw(6,2,first 24,fi 10)", false, 6, 2, 24, 10, 0, 10, 11, 1},
      {"ind draw(6,3,first 24,fi 10)", false, 6, 3, 24, 10, 0, 12, 11, 1},
      {"ind indexed(6,2,idx 4..9,vo 20,fi 10)", true, 6, 2, 0, 10, 20, 10, 11, 1},
      {"ind x2 draw(6,1,24,fi 12)+(6,1,0,fi 13)", false, 6, 1, 24, 12, 0, 12, 13, 2},
      {"", false, 6, 1, 0, 13, 0, 0, 0, 0},
      {"ind x2 indexed(..,fi 10)+(..,fi 14)", true, 6, 2, 0, 10, 20, 14, 15, 2},
      {"", true, 6, 2, 0, 14, 20, 0, 0, 0},
   };
   /* The indirect commands, one slot per case (stride 32 covers both
    * kinds). */
   struct buf ind = make_buffer(32 * sizeof(cases) / sizeof(cases[0]),
                                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, NULL);
   for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
      uint32_t *w = (uint32_t *)((uint8_t *)ind.map + 32 * c);
      const struct draw_case *d = &cases[c];
      if (d->indexed) {
         const VkDrawIndexedIndirectCommand k = {d->count, d->instances, d->first,
                                                 d->vertex_offset, d->first_instance};
         memcpy(w, &k, sizeof(k));
      } else {
         const VkDrawIndirectCommand k = {d->count, d->instances, d->first, d->first_instance};
         memcpy(w, &k, sizeof(k));
      }
   }
   int fails = 0, ran = 0;
   for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
      const struct draw_case *d = &cases[c];
      if (!d->name[0] || (d->indirect && !indirect))
         continue;
      ran++;
      CHECK(vkResetCommandBuffer(cb, 0));
      VkCommandBufferBeginInfo bi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK(vkBeginCommandBuffer(cb, &bi));
      VkClearValue clear = {.color = {.float32 = {0, 0, 0, 0}}};
      VkRenderPassBeginInfo rpbi = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
         .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1,
         .pClearValues = &clear,
      };
      vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      VkBuffer bufs[2] = {vbuf.b, tbuf.b};
      VkDeviceSize offs[2] = {0, 0};
      vkCmdBindVertexBuffers(cb, 0, 2, bufs, offs);
      if (d->indexed)
         vkCmdBindIndexBuffer(cb, ibuf.b, 0, VK_INDEX_TYPE_UINT16);
      if (d->indirect) {
         const uint32_t n = d->indirect == 2 ? 2 : 1;
         if (d->indexed)
            vkCmdDrawIndexedIndirect(cb, ind.b, 32 * c, n, 32);
         else
            vkCmdDrawIndirect(cb, ind.b, 32 * c, n, 32);
      } else if (d->indexed) {
         vkCmdDrawIndexed(cb, d->count, d->instances, d->first, d->vertex_offset,
                          d->first_instance);
      } else {
         vkCmdDraw(cb, d->count, d->instances, d->first, d->first_instance);
      }
      vkCmdEndRenderPass(cb);
      VkBufferImageCopy reg = {
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {W, H, 1},
      };
      vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &reg);
      VkMemoryBarrier mb = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      };
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &mb, 0, NULL, 0, NULL);
      CHECK(vkEndCommandBuffer(cb));
      VkSubmitInfo si = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb,
      };
      CHECK(vkResetFences(dev, 1, &fence));
      CHECK(vkQueueSubmit(q, 1, &si, fence));
      CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));

      const uint8_t *px = rb.map;
      int left = px[(H / 2 * W + W / 4) * 4], right = px[(H / 2 * W + 3 * W / 4) * 4];
      int el = d->expect_left < 0 ? 0 : d->expect_left;
      int er2 = d->expect_right < 0 ? 0 : d->expect_right;
      bool ok = left == el && right == er2;
      printf("%-36s left %3d right %3d  expect %3d %3d  %s\n", d->name, left, right, el, er2,
             ok ? "ok" : "WRONG");
      fails += !ok;
   }
   printf("%s: %d of %d cases wrong\n", fails ? "FAIL" : "PASS", fails, ran);
   vkDeviceWaitIdle(dev);
   return fails ? 1 : 0;
}
