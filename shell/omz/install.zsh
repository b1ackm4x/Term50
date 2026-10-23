#!/usr/bin/env zsh
emulate -LR zsh
setopt ERR_EXIT NO_UNSET
umask 077
[[ -n ${SANDBOX:-} && -d $SANDBOX/data && -d ${1:-}/oh-my-zsh ]] || exit 2
bundle=$1
"$bundle/runtime/bin/zsh" -f "$bundle/prepare.zsh" "$bundle"
for required in oh-my-zsh/oh-my-zsh.sh custom/plugins/zsh-autosuggestions/zsh-autosuggestions.zsh custom/plugins/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh custom/functions/compinit runtime/bin/git runtime/bin/zsh runtime/share/zsh/5.9/functions/compdump runtime/share/zsh/5.9/functions/term50_compinit_core init.zsh warm-cache.zsh; do
  [[ -r $bundle/$required ]] || exit 4
done
"$bundle/runtime/bin/zsh" -n "$bundle/init.zsh"
shell_dir="$SANDBOX/data/shell"
rc_file="$HOME/.zshrc"
[[ ! -L $rc_file && ! -L $shell_dir ]] || exit 3
mkdir -p "$shell_dir/backups" "$shell_dir/omz-cache"
backup="$shell_dir/backups/before-oh-my-zsh-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$backup"
[[ ! -f $rc_file ]] || cp -p "$rc_file" "$backup/zshrc"
[[ ! -f $HOME/.term.lua ]] || cp -p "$HOME/.term.lua" "$backup/term.lua"
[[ ! -f $shell_dir/history.zsh ]] || cp -p "$shell_dir/history.zsh" "$backup/history.zsh"
typeset -a moved
rollback() {
  local pair from dest
  for pair in "${moved[@]}"; do
    from=${pair%%:*}; dest="$SANDBOX/data/${pair#*:}"
    [[ ! -e $dest ]] || mv "$dest" "$bundle/$from"
    [[ ! -e $backup/$from ]] || mv "$backup/$from" "$dest"
  done
  [[ ! -f $backup/omz-init.zsh ]] || cp -p "$backup/omz-init.zsh" "$shell_dir/omz-init.zsh"
  [[ ! -f $backup/zshrc ]] || cp -p "$backup/zshrc" "$rc_file"
}
trap 'rollback' ZERR
for pair in 'oh-my-zsh:oh-my-zsh' 'custom:shell/omz-custom' 'runtime:omz-runtime'; do
  from=${pair%%:*}
  dest="$SANDBOX/data/${pair#*:}"
  [[ ! -L $dest ]] || exit 3
  [[ ! -e $dest ]] || mv "$dest" "$backup/$from"
  moved+=("$pair")
  mv "$bundle/$from" "$dest"
done
[[ ! -f $shell_dir/omz-init.zsh ]] || cp -p "$shell_dir/omz-init.zsh" "$backup/omz-init.zsh"
cp "$bundle/init.zsh" "$shell_dir/omz-init.zsh"
chmod 600 "$shell_dir/omz-init.zsh"
# Put the complete original contents first and add only one loader block.
content=''
[[ ! -f $rc_file ]] || content=$(<"$rc_file")
if [[ $content != *'# BEGIN TERM50 OH MY ZSH'* ]]; then
  [[ ! -f $rc_file ]] || cp -p "$rc_file" "$rc_file.omz-new"
  cat >> "$rc_file.omz-new" <<'RC'

# BEGIN TERM50 OH MY ZSH
ZSH_THEME="agnoster"
[[ ! -r $SANDBOX/data/shell/omz-init.zsh ]] || source "$SANDBOX/data/shell/omz-init.zsh"
# END TERM50 OH MY ZSH
RC
  chmod 600 "$rc_file.omz-new"
  "$SANDBOX/app/native/bbnix/bin/zsh" -n "$rc_file.omz-new"
  mv "$rc_file.omz-new" "$rc_file"
fi
print -r -- "$backup" > "$shell_dir/omz-backup-path"
chmod 600 "$shell_dir/omz-backup-path"
"$SANDBOX/data/omz-runtime/bin/zsh" -dfi "$bundle/warm-cache.zsh"
trap - ZERR
print OH_MY_ZSH_INSTALLED_WITH_BACKUP
