#include <vulkan/vulkan.h>
#include <stdio.h>
int main(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &ai;
    VkInstance inst;
    VkResult r = vkCreateInstance(&ic, NULL, &inst);
    printf("vkCreateInstance=%d\n", (int)r);
    if (r == VK_SUCCESS) vkDestroyInstance(inst, NULL);
    return 0;
}
