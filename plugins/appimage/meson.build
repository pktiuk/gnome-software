cargs = ['-DG_LOG_DOMAIN="GsPluginAppImage"']
cargs += ['-DLOCALPLUGINDIR="' + meson.current_build_dir() + '"']
cargs += ['-DLOCALPLUGINDIR_CORE="' + meson.current_build_dir() + '/../core"']

shared_module(
  'gs_plugin_appimage',
  sources : [
    'gs-plugin-appimage.c',
    'gs-appimage-utils.c'
    ],
  include_directories : [
    include_directories('../..'),
    include_directories('../../lib'),
  ],
  install : true,
  install_dir: plugin_dir,
  c_args : cargs,
  dependencies : [plugin_libs, libappimage],
)

