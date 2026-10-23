#!/usr/bin/env zsh
# Run once with the native Zsh after installing the loader.
emulate -LR zsh
setopt ERR_EXIT
umask 077
[[ -o interactive && -n ${SANDBOX:-} ]] || exit 2
HISTFILE="$SANDBOX/data/shell/omz-cache/warmup-history"
source "$HOME/.zshrc"
[[ $_comps[git] == _git && -s $ZSH_CACHE_DIR/compaudit-ok-$ZSH_VERSION ]] || exit 3
for file in "$ZSH"/lib/*.zsh "$ZSH"/themes/agnoster.zsh-theme \
  "$ZSH_CUSTOM"/plugins/*/*.zsh \
  "$ZSH_CUSTOM"/plugins/zsh-syntax-highlighting/highlighters/*/*.zsh; do
  zcompile -U -R "$file.zwc" "$file"
done
unset HISTFILE
command rm -f "$SANDBOX/data/shell/omz-cache/warmup-history"
print OH_MY_ZSH_PRIVATE_CACHE_READY
