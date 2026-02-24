project(
  'vulkanshit',
  'cpp',
  version : '0.1',
  meson_version : '>= 1.3.0',
  default_options : ['warning_level=3', 'cpp_std=c++20'],
)

wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
spdlog = dependency('spdlog')
vulkan = dependency('vulkan')

dependencies = [
  wayland_client,
  wayland_protocols,
  spdlog,
  vulkan,
]

sources = [
  'src/vulkanshit.cpp',
]

exe = executable(
  'vulkanshit',
  sources,
  dependencies : dependencies,
)
