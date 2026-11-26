"""Classify native image handle negotiation without altering Vulkan input."""
def compatible(vk_format, external_format, handle_types):
    if external_format:
        return vk_format == 0 and external_format in (24,28,29) and bool(handle_types & (0x2000|0x8000))
    return vk_format != 0 and bool(handle_types & 1)
