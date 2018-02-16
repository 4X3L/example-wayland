project('example_wayland', 'c', default_options : 'c_std=gnu11')
dependency('wayland-client')
dependency('xkbcommon')
c_flags = ['-g', '-Og']
ld_flags = ['-lwayland-client', '-lrt', '-lxkbcommon', '-lm']
src = ['src/main.c', 'src/xdg-shell-unstable-v6-client-protocol.c', 'src/int_set.c']
executable('example_wayland', src, c_args : c_flags, link_args : ld_flags)