project('example_wayland', 'c', default_options : 'c_std=gnu99')
dependency('wayland-client')
c_flags = ['-Wall', '-Wextra', '-pedantic', '-Werror', '-ggdb3', '-Og']
ld_flags = ['-lwayland-client', '-lrt']
src = ['src/main.c']
executable('example_wayland', src, c_args : c_flags, link_args : ld_flags)