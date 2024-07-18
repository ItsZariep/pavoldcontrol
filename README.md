# Pavoldcontrol
Pavucontrol fork that still uses gtk3 (Based on <a href="https://github.com/pulseaudio/pavucontrol/commit/52aef7e81d9c44601e78bac4930084b513a2936e">2aef7e</a>)

### Why?
For some users or context, it is more comfortable to use the version with GTK3 instead of GTK4, for example systems that do not use GTK4 at all, or users who simply do not have it configured so that the theme fits, in my case, I use Wayfire and Pavucontrol GTK4 gives me a lot of problems with window decoration.

### Is Updated?
I'm going to try to backport the most important commits from the main project, but as long as the program works, I most likely won't touch it.

## Build
```
 meson setup build
```
```
meson compile -C build
```


