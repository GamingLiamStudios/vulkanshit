project(
  'vulkanshit',
  'cpp',
  'c',
  version: '0.1',
  meson_version: '>= 1.3.0',
  default_options: ['warning_level=3', 'cpp_std=c++20', 'c_std=c99'],
)

wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
spdlog = dependency('spdlog')
vulkan = dependency('vulkan')

wl_mod = import('wayland')
xdg_xml = wl_mod.find_protocol('xdg-shell')
xdg_shell = wl_mod.scan_xml(xdg_xml)

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
  xdg_shell,
  dependencies: dependencies,
)