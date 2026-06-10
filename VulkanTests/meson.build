project('VulkanTests', 'cpp',
   default_options: ['cpp_std=c++20'])

dep_catch2 = dependency('catch2-with-main')
dep_vulkan = dependency('vulkan')

vulkan_test_common = static_library('vulkan_test_common',
   'VulkanFixture.cpp',
   dependencies: [dep_vulkan],
   gnu_symbol_visibility: 'hidden',
)

# A single Catch2 executable bundles every test_*.cpp. Adding a new test
# means dropping a file here and listing it in this array — no boilerplate.
test_sources = [
   'test_opaque_fd.cpp',
   'test_timeline_semaphore_opaque_fd.cpp',
   'test_external_memory_host.cpp',
]

executable('VulkanTests',
   sources: test_sources,
   link_with: vulkan_test_common,
   dependencies: [dep_catch2, dep_vulkan],
   gnu_symbol_visibility: 'hidden',
   install: true,
)
