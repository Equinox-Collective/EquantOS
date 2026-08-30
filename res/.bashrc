# EquantOS Bash Environment & Aliases
EQ_APPLETS="ls cat grep head tail tr wc sort uniq cut uname mkdir rmdir rm cp mv touch basename dirname env seq sleep yes tee rev date whoami hostname du df sync clear xxd hexdump md5sum sha256sum expr which find sed awk cal free uptime"

for a in $EQ_APPLETS; do
    alias $a="/busybox.elf $a"
done

unset a EQ_APPLETS

# Красивый цветной Prompt (зеленый root@equantos)
PS1='\[\033[01;32m\]root@equantos\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]# '