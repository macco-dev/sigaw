project(
  'sigaw',
  ['c', 'cpp'],
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c17',
    'cpp_std=c++20',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
)

prefix_dir = get_option('prefix')
libdir_abs = get_option('libdir')
if not libdir_abs.startswith('/')
  libdir_abs = prefix_dir / libdir_abs
endif

bindir_abs = get_option('bindir')
if not bindir_abs.startswith('/')
  bindir_abs = prefix_dir / bindir_abs
endif

datadir_abs = get_option('datadir')
if not datadir_abs.startswith('/')
  datadir_abs = prefix_dir / datadir_abs
endif

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# Vulkan (headers + loader)
vulkan_dep = dependency('vulkan', required: true)

# POSIX threads
thread_dep = dependency('threads', required: true)

# POSIX realtime (for shm_open)
rt_dep = cc.find_library('rt', required: true)

# nlohmann/json (daemon only)
# Try system install first, fall back to subproject
json_dep = dependency('nlohmann_json', required: false)
if not json_dep.found()
  json_dep = dependency('nlohmann_json',
    fallback: ['nlohmann_json', 'nlohmann_json_dep'],
    required: true
  )
endif

# libcurl (daemon, required for OAuth2 token exchange)
curl_dep = dependency('libcurl', required: true)

# Layer text/image rendering
freetype_dep = dependency('freetype2', required: true)
png_dep = dependency('libpng', required: true)

# --------------------------------------------------------------------------
# Vulkan Layer (libSigaw.so)
# --------------------------------------------------------------------------

layer_sources = [
  'src/layer/layer_entry.c',
  'src/layer/overlay_renderer.cpp',
]

layer_cpp_args = [
  '-fPIC',
  '-fvisibility=hidden',
  '-DSIGAW_DATA_DIR="' + (datadir_abs / 'sigaw') + '"',
  '-DSIGAW_SOURCE_DIR="' + meson.current_source_dir() + '"',
]

layer_lib = shared_library(
  'Sigaw',
  layer_sources,
  include_directories: include_directories('src'),
  dependencies: [vulkan_dep, rt_dep, thread_dep, freetype_dep, png_dep],
  install: true,
  # Layer .so must be position-independent
  c_args: ['-fPIC', '-fvisibility=hidden'],
  cpp_args: layer_cpp_args,
  # Export only the Vulkan layer entry points
  link_args: [
    '-Wl,--version-script,' + meson.current_source_dir() / 'layer.map',
  ],
)

layer_manifest_conf = configuration_data()
layer_manifest_conf.set('layer_library_path', libdir_abs / 'libSigaw.so')
layer_manifest = configure_file(
  input: 'layer-manifest/VK_LAYER_MACCO_sigaw.x86_64.json.in',
  output: 'VK_LAYER_MACCO_sigaw.x86_64.json',
  configuration: layer_manifest_conf,
)

install_data(
  layer_manifest,
  install_dir: get_option('datadir') / 'vulkan' / 'implicit_layer.d',
)

service_conf = configuration_data()
service_conf.set('sigaw_daemon_path', bindir_abs / 'sigaw-daemon')
sigaw_service = configure_file(
  input: 'contrib/sigaw-daemon.service.in',
  output: 'sigaw-daemon.service',
  configuration: service_conf,
)

install_data(
  sigaw_service,
  install_dir: get_option('datadir') / 'systemd' / 'user',
)

install_data(
  'sigaw.conf.example',
  install_dir: get_option('datadir') / 'doc' / 'sigaw',
)

install_data(
  [
    'assets/fonts/NotoSans-Medium.ttf',
    'assets/fonts/NotoSans-Bold.ttf',
    'assets/fonts/NotoSans-LICENSE',
  ],
  install_dir: get_option('datadir') / 'sigaw' / 'fonts',
)

# --------------------------------------------------------------------------
# Daemon (sigaw-daemon)
# --------------------------------------------------------------------------

daemon_sources = [
  'src/daemon/main.cpp',
]

daemon_deps = [rt_dep, thread_dep, json_dep, curl_dep]

daemon_bin = executable(
  'sigaw-daemon',
  daemon_sources,
  include_directories: include_directories('src'),
  dependencies: daemon_deps,
  install: true,
)

# --------------------------------------------------------------------------
# Control CLI (sigaw-ctl)
# --------------------------------------------------------------------------

ctl_bin = executable(
  'sigaw-ctl',
  'src/ctl/main.cpp',
  include_directories: include_directories('src'),
  dependencies: [rt_dep],
  install: true,
)

test(
  'sigaw-daemon-help',
  daemon_bin,
  args: ['--help'],
)

test(
  'sigaw-daemon-init-config',
  daemon_bin,
  args: ['--init-config'],
  env: ['XDG_CONFIG_HOME=' + (meson.current_build_dir() / 'test-config')],
)

test(
  'sigaw-ctl-help',
  ctl_bin,
  args: ['help'],
)

json_utils_test = executable(
  'sigaw-json-utils-test',
  'tests/json_utils_test.cpp',
  include_directories: include_directories('src'),
  dependencies: [json_dep],
)

test(
  'sigaw-json-utils',
  json_utils_test,
)

overlay_layout_test = executable(
  'sigaw-overlay-layout-test',
  'tests/overlay_layout_test.cpp',
  include_directories: include_directories('src'),
)

test(
  'sigaw-overlay-layout',
  overlay_layout_test,
)

overlay_visibility_test = executable(
  'sigaw-overlay-visibility-test',
  'tests/overlay_visibility_test.cpp',
  include_directories: include_directories('src'),
)

test(
  'sigaw-overlay-visibility',
  overlay_visibility_test,
)

daemon_voice_runtime_test = executable(
  'sigaw-daemon-voice-runtime-test',
  'tests/daemon_voice_runtime_test.cpp',
  include_directories: include_directories('src'),
  dependencies: [json_dep, curl_dep],
)

test(
  'sigaw-daemon-voice-runtime',
  daemon_voice_runtime_test,
)

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------

summary({
  'Vulkan layer': true,
  'Daemon': true,
  'CLI tool': true,
  'libcurl (OAuth)': curl_dep.found(),
  'nlohmann/json': json_dep.found(),
  'FreeType': freetype_dep.found(),
  'libpng': png_dep.found(),
}, section: 'Components')
