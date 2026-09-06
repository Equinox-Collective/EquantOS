# 1. Enable alias expansion in non-interactive scripts
shopt -s expand_aliases

# 2. System Environment Variables
export TERM="linux"
export HOME="/"
export USER="root"
export PATH="/bin:/usr/bin:/sys/bin:/"

# 3. BusyBox Multi-Call Applets Setup
EQ_BUSYBOX_APPLETS="ls cat grep head tail tr wc sort uniq cut uname mkdir rmdir rm cp mv touch basename dirname env seq sleep yes tee rev date whoami hostname du df sync clear xxd hexdump md5sum sha256sum expr which find sed awk cal free uptime vi ps kill dmesg"

for app in $EQ_BUSYBOX_APPLETS; do
    alias $app="/busybox.elf $app"
done
unset app EQ_BUSYBOX_APPLETS

# 4. EquantOS Kernel Diagnostic Bridge Applets (kdiag)
EQ_KERNEL_TOOLS="pciscan mem memstress diskinfo hexdump sysinfo ataread nvmeread atastress nvmestress fstest gptdump mbrdump devtest cpuinfo vmstress pmmbench schedtest msrtest ioperf fatinfo ext2info xhcitest pcipeek inbtest installer colortest fonttest ttytest mkfstest su passwd useradd"

for tool in $EQ_KERNEL_TOOLS; do
    alias $tool="/bin/kdiag $tool"
done
unset tool EQ_KERNEL_TOOLS

# 5. Colorful High-Contrast Prompt (root in Green, cwd in Blue)
PS1='\[\033[01;32m\]root@equantos\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]# '

# 6. Welcome Banner
echo -e "\033[36mEquantOS Unix Subsystem [GNU Bash 5.2 + BusyBox Core]\033[0m"
echo -e "Type '\033[32mhelp\033[0m' or '\033[33mpciscan\033[0m' to inspect system status.\n"