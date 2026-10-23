# Term50 history picker. Source from an interactive zsh; no external plugins.
[[ -o interactive ]] || return 0
zmodload zsh/parameter || return 1

# Initialize phone history; a remote host keeps its existing history policy.
if [[ -n ${SANDBOX:-} && -d $SANDBOX/data ]]; then
  if [[ -z ${HISTFILE:-} ]]; then
    ( umask 077; mkdir -p "$SANDBOX/data/shell"; [[ -e $SANDBOX/data/shell/history ]] || : > "$SANDBOX/data/shell/history" ) || return 1
    HISTFILE="$SANDBOX/data/shell/history"
  fi
  (( HISTSIZE < 10000 )) && HISTSIZE=10000
  (( SAVEHIST < 5000 )) && SAVEHIST=5000
  setopt APPEND_HISTORY INC_APPEND_HISTORY HIST_IGNORE_DUPS HIST_IGNORE_SPACE
fi

_term50_history_filter() {
  emulate -L zsh
  local entry
  _th_matches=()
  for entry in "${_th_entries[@]}"; do
    [[ ${(L)entry} == *"${(L)BUFFER}"* ]] && _th_matches+=("$entry")
  done
  _th_selected=1
  _term50_history_draw
}

_term50_history_draw() {
  emulate -L zsh
  local line marker out
  local -i count=${#_th_matches} rows=$(( LINES - 6 )) width=$(( COLUMNS - 4 )) first last i
  (( rows > 8 )) && rows=8
  (( rows < 1 )) && rows=1
  (( width < 8 )) && width=8
  (( _th_selected > count && count )) && _th_selected=$count
  (( _th_selected < 1 )) && _th_selected=1
  first=$(( ((_th_selected - 1) / rows) * rows + 1 ))
  last=$(( first + rows - 1 ))
  (( last > count )) && last=$count
  out=$'\nHistory: type to filter | Up/Down select\nEnter: insert | Esc: cancel'
  out+=$'\n'"[$((count ? _th_selected : 0))/$count]"
  for (( i=first; i<=last; i++ )); do
    # Make control characters visible, never emit history as terminal escapes.
    line=${(V)_th_matches[i]}
    (( ${#line} > width )) && line="${line[1,$((width-3))]}..."
    marker='  '
    (( i == _th_selected )) && marker='> '
    out+=$'\n'"$marker$line"
  done
  (( count )) || out+=$'\n  No matching history'
  _th_panel=$out
  POSTDISPLAY=$_th_panel
  zle -R
}

# Autosuggestion plugins also draw in POSTDISPLAY. Keep this panel visible
# while the picker owns the editor, including asynchronous suggestion replies.
_term50_history_redraw_guard() {
  if (( ${_th_active:-0} )); then
    POSTDISPLAY=$_th_panel
  fi
  return 0
}

_term50_history_type() { zle .self-insert; _term50_history_filter; }
_term50_history_erase() { zle .backward-delete-char; _term50_history_filter; }
_term50_history_clear() { BUFFER=''; CURSOR=0; _term50_history_filter; }
_term50_history_up() { (( --_th_selected )); _term50_history_draw; }
_term50_history_down() { (( ++_th_selected )); _term50_history_draw; }
_term50_history_accept() {
  if (( ${#_th_matches} )); then
    _th_choice=${_th_matches[_th_selected]}
    _th_accepted=1
    zle .accept-line
  else
    zle beep
  fi
}

_term50_history_picker() {
  emulate -L zsh
  local _th_original=$BUFFER _th_display=$POSTDISPLAY _th_keymap=$KEYMAP
  local _th_choice='' _th_panel='' event entry
  local -i _th_cursor=$CURSOR _th_selected=1 _th_accepted=0 _th_active=1
  local -a _th_entries=() _th_matches=()
  local -A seen=()
  # Include commands written by other tabs, without printing or parsing history.
  [[ -r $HISTFILE ]] && fc -RI "$HISTFILE"
  for event in ${(Onk)history}; do
    entry=${history[$event]}
    [[ -z $entry || -n ${seen[$entry]-} ]] && continue
    seen[$entry]=1
    _th_entries+=("$entry")
    (( ${#_th_entries} >= 5000 )) && break
  done
  {
    _term50_history_filter
    zle -K term50-history
    zle .recursive-edit
  } always {
    _th_active=0
    POSTDISPLAY=$_th_display
    zle -K "$_th_keymap"
    if (( _th_accepted )); then
      POSTDISPLAY=''
      BUFFER=$_th_choice
      CURSOR=${#BUFFER}
    else
      BUFFER=$_th_original
      CURSOR=$_th_cursor
    fi
    zle -R
  }
  return 0
}

zle -N term50-history _term50_history_picker
zle -N term50-history-type _term50_history_type
zle -N term50-history-erase _term50_history_erase
zle -N term50-history-clear _term50_history_clear
zle -N term50-history-up _term50_history_up
zle -N term50-history-down _term50_history_down
zle -N term50-history-accept _term50_history_accept
bindkey -N term50-history
bindkey -M term50-history -R ' '-'~' term50-history-type
bindkey -M term50-history -R $'\x80'-$'\xff' term50-history-type
bindkey -M term50-history '^M' term50-history-accept
bindkey -M term50-history '^J' term50-history-accept
bindkey -M term50-history '^?' term50-history-erase
bindkey -M term50-history '^H' term50-history-erase
bindkey -M term50-history '^U' term50-history-clear
bindkey -M term50-history '^P' term50-history-up
bindkey -M term50-history '^N' term50-history-down
bindkey -M term50-history '^R' term50-history-down
bindkey -M term50-history '^C' send-break
bindkey -M term50-history '^G' send-break
bindkey -M term50-history '^[' send-break
bindkey -M term50-history '^[[A' term50-history-up
bindkey -M term50-history '^[[B' term50-history-down
bindkey -M term50-history '^[OA' term50-history-up
bindkey -M term50-history '^[OB' term50-history-down
bindkey -M emacs '^R' term50-history
bindkey -M viins '^R' term50-history
bindkey -M vicmd '^R' term50-history

if (( ${+ZSH_AUTOSUGGEST_IGNORE_WIDGETS} )); then
  if (( ! ${ZSH_AUTOSUGGEST_IGNORE_WIDGETS[(Ie)term50-history-*]} )); then
    ZSH_AUTOSUGGEST_IGNORE_WIDGETS+=('term50-history-*')
  fi
  autoload -Uz add-zle-hook-widget
  add-zle-hook-widget line-pre-redraw _term50_history_redraw_guard
fi
