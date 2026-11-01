// Minimal Vulkan compute runner for the translator tests: vkrun shader.spv out.bin outBytes threads [binding:file ...]
// Bindings 0/1 are uniform buffers, others storage buffers; the output is binding 41.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed %d\n", #x, r_); exit(1); } } while (0)
static std::vector<char> ReadFile(const char* p) { FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(1); } fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); std::vector<char> d(n); if (fread(d.data(), 1, n, f) != size_t(n)) exit(1); fclose(f); return d; }
int main(int argc, char** argv)
{
    if (argc < 5) return 2;
    const uint32_t outBytes = atoi(argv[3]), threads = atoi(argv[4]);
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.apiVersion = VK_API_VERSION_1_2; ici.pApplicationInfo = &ai;
    VkInstance inst; CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr); std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
    VkPhysicalDevice pd = pds[0];
    float prio = 1; VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qci.queueCount = 1; qci.pQueuePriorities = &prio; qci.queueFamilyIndex = 0;
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.runtimeDescriptorArray = VK_TRUE; f12.descriptorBindingPartiallyBound = VK_TRUE; f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 }; f2.pNext = &f12; f2.features.shaderInt64 = VK_TRUE;
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.pNext = &f2;
    VkDevice dev; CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    struct Buf { uint32_t binding; VkBuffer b; VkDeviceMemory m; uint32_t size; bool uniform; void* map; };
    std::vector<Buf> bufs;
    auto make = [&](uint32_t binding, const std::vector<char>& init, uint32_t size) {
        Buf B{ binding, {}, {}, size, binding < 2, nullptr };
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bci.size = size;
        bci.usage = B.uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        CK(vkCreateBuffer(dev, &bci, nullptr, &B.b));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, B.b, &mr);
        uint32_t ti = 0; for (; ti < mp.memoryTypeCount; ti++) if ((mr.memoryTypeBits >> ti & 1) && (mp.memoryTypes[ti].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; mai.allocationSize = mr.size; mai.memoryTypeIndex = ti;
        CK(vkAllocateMemory(dev, &mai, nullptr, &B.m)); CK(vkBindBufferMemory(dev, B.b, B.m, 0));
        CK(vkMapMemory(dev, B.m, 0, size, 0, &B.map)); memset(B.map, 0, size); memcpy(B.map, init.data(), std::min<size_t>(init.size(), size));
        bufs.push_back(B);
    };
    for (int i = 5; i < argc; i++)
    {
        std::string a = argv[i]; size_t c = a.find(':');
        uint32_t binding = atoi(a.substr(0, c).c_str()); auto d = ReadFile(a.substr(c + 1).c_str());
        make(binding, d, std::max<uint32_t>(uint32_t(d.size() + 15) & ~15u, 65536));
    }
    make(41, {}, outBytes);
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (auto& B : bufs) lb.push_back({ B.binding, B.uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; lci.bindingCount = lb.size(); lci.pBindings = lb.data();
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &dsl));
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));
    auto code = ReadFile(argv[1]);
    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; smci.codeSize = code.size(); smci.pCode = (const uint32_t*)code.data();
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &smci, nullptr, &sm));
    VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO }; cpci.layout = pl;
    cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "cs_main", nullptr };
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 } };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dp));
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dsai.descriptorPool = dp; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    std::vector<VkDescriptorBufferInfo> infos(bufs.size()); std::vector<VkWriteDescriptorSet> ws;
    for (size_t i = 0; i < bufs.size(); i++)
    {
        infos[i] = { bufs[i].b, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w.dstSet = ds; w.dstBinding = bufs[i].binding; w.descriptorCount = 1;
        w.descriptorType = bufs[i].uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &infos[i]; ws.push_back(w);
    }
    vkUpdateDescriptorSets(dev, ws.size(), ws.data(), 0, nullptr);
    VkCommandPoolCreateInfo cpi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, nullptr, &cp));
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cbai.commandPool = cp; cbai.commandBufferCount = 1; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; CK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    vkCmdDispatch(cb, threads, 1, 1);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    FILE* f = fopen(argv[2], "wb"); fwrite(bufs.back().map, 1, outBytes, f); fclose(f);
    // Also dump storage buffer 40 (memory export target) if present.
    for (auto& B : bufs) if (B.binding == 40) { FILE* g = fopen((std::string(argv[2]) + ".u0").c_str(), "wb"); fwrite(B.map, 1, B.size, g); fclose(g); }
    return 0;
}
