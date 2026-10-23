#!/usr/bin/env zsh
# Install the same picker for SSH Zsh sessions, retaining the host's history.
emulate -LR zsh
setopt ERR_EXIT NO_UNSET
umask 077
[[ -f ${1:-} ]] || exit 2
source_file=$1
destination="$HOME/.local/share/term50-shell"
rc_file="${ZDOTDIR:-$HOME}/.zshrc"
[[ ! -L $destination && ! -L $rc_file ]] || exit 3
mkdir -p "$destination/backups"
chmod 700 "$destination" "$destination/backups"
backup="$destination/backups/zshrc.$(date +%Y%m%d-%H%M%S).$$"
[[ ! -f $rc_file ]] || { cp -p "$rc_file" "$backup"; chmod 600 "$backup"; }
[[ ! -f $destination/history.zsh ]] || cp -p "$destination/history.zsh" "$backup.history-plugin"
cp "$source_file" "$destination/history.zsh.new"
chmod 600 "$destination/history.zsh.new"
zsh -n "$destination/history.zsh.new"
mv "$destination/history.zsh.new" "$destination/history.zsh"
content=''
[[ ! -f $rc_file ]] || content=$(<"$rc_file")
if [[ $content != *'# BEGIN TERM50 SSH HISTORY PICKER'* ]]; then
  print -r -- "$content" > "$rc_file.history-new"
  cat >> "$rc_file.history-new" <<'RC'

# BEGIN TERM50 SSH HISTORY PICKER
if [[ -n ${SSH_CONNECTION:-} && -r $HOME/.local/share/term50-shell/history.zsh ]]; then
  source "$HOME/.local/share/term50-shell/history.zsh"
fi
# END TERM50 SSH HISTORY PICKER
RC
  chmod 600 "$rc_file.history-new"
  zsh -n "$rc_file.history-new"
  mv "$rc_file.history-new" "$rc_file"
fi
print 'SSH_HISTORY_INSTALLED_WITH_BACKUP'
