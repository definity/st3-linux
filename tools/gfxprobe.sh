#!/bin/sh
# Run this in the terminal you'll use for st3. It shows three coloured boxes, one per graphics protocol.
printf 'Terminal graphics probe -- tell Claude which colours you see.\n\n'
printf '1) SIXEL (should be a RED box):\n'
printf '%s\n' 'Pq#1;2;100;0;0#1!48~-#1!48~-#1!48~-#1!48~-\'
printf '\n2) iTerm2 inline image (should be a GREEN box):\n'
printf '\033]1337;File=inline=1;width=48px;height=24px;preserveAspectRatio=0:iVBORw0KGgoAAAANSUhEUgAAADAAAAAYCAIAAAAzn+mLAAAAMElEQVR4nO3OQQkAMBADsPpXNlkzcdA+AhGQ5GVLfyAkJCQ0pj8QEhISGtMfCAnd+qQShC5+aKaeAAAAAElFTkSuQmCC\a\n'
printf '\n3) kitty graphics (should be a BLUE box):\n'
printf '\033_Ga=T,f=100;iVBORw0KGgoAAAANSUhEUgAAADAAAAAYCAIAAAAzn+mLAAAAMUlEQVR4nO3OQQ0AMAgEMOQgB/9mNhccjyYV0Op5p1R8ICQkJJQeCAkJCaUHQkJCyz6gDJfbmzKIcwAAAABJRU5ErkJggg==\033\\\n'
printf '\n\nDone. Device attributes reply (may be blank): '
old=$(stty -g); stty raw -echo min 0 time 5; printf '\033[c'; r=$(dd bs=64 count=1 2>/dev/null); stty "$old"
printf '%s\n' "$r" | od -c | head -2
