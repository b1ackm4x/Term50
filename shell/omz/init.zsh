# Oh My Zsh integration for the Term50 QNX runtime.
[[ -o interactive && -n ${SANDBOX:-} ]] || return 0
export ZSH="$SANDBOX/data/oh-my-zsh"
ZSH_CUSTOM="$SANDBOX/data/shell/omz-custom"
ZSH_CACHE_DIR="$SANDBOX/data/shell/omz-cache"
ZSH_COMPDUMP="$ZSH_CACHE_DIR/zcompdump-$ZSH_VERSION"
# BB10 application UIDs have no passwd/group name; avoid empty-name lookups.
: ${USER:=q20}
: ${LOGNAME:=$USER}
export USER LOGNAME
export COLORTERM=truecolor
export TERM=xterm-256color
# Keep the existing private history file and unified picker.
: ${HISTFILE:="$SANDBOX/data/shell/history"}
typeset -U path fpath
# BB10 installed assets belong to apps, so compaudit requires a private copy.
local -a term50_extra_fpath=( ${fpath:#$SANDBOX/app/native/bbnix/share/zsh/*} )
term50_extra_fpath=( ${term50_extra_fpath:#$ZSH/*} )
term50_extra_fpath=( ${term50_extra_fpath:#$ZSH_CUSTOM/*} )
term50_extra_fpath=( ${term50_extra_fpath:#$ZSH_CACHE_DIR/*} )
fpath=("$SANDBOX/data/omz-runtime/share/zsh/5.9/functions" $term50_extra_fpath)
unset term50_extra_fpath
path=("$SANDBOX/data/omz-runtime/bin" $path)
zstyle ':omz:update' mode disabled
zstyle ':omz:alpha:lib:git' async-prompt no
plugins=(git zsh-autosuggestions zsh-syntax-highlighting)
ZSH_AUTOSUGGEST_HIGHLIGHT_STYLE='fg=244'
ZSH_AUTOSUGGEST_STRATEGY=(history)
ZSH_AUTOSUGGEST_BUFFER_MAX_SIZE=200
# All widgets are installed before the first prompt. Avoid rebuilding every
# widget wrapper (and spawning a listing subshell) for each command on BB10.
ZSH_AUTOSUGGEST_MANUAL_REBIND=1
# Avoid background suggestions competing with the history picker on QNX.
unset ZSH_AUTOSUGGEST_USE_ASYNC
AGNOSTER_STATUS_RETVAL_NUMERIC=true
AGNOSTER_GIT_BRANCH_STATUS=false
: ${ZSH_THEME:=agnoster}
# A portable dump of the identical pinned completion files avoids the slow
# first scan on BB10. OMZ validates revision/fpath on each start; the private
# compinit wrapper performs the ownership/file-count audit initially and daily.
if [[ ! -s $ZSH_COMPDUMP && -r $SANDBOX/data/omz-runtime/share/zsh/5.9/completion-seed ]]; then
  cp "$SANDBOX/data/omz-runtime/share/zsh/5.9/completion-seed" "$ZSH_COMPDUMP"
  local -a term50_seed_fpath=(
    "$ZSH_CUSTOM/plugins/zsh-syntax-highlighting"
    "$ZSH_CUSTOM/plugins/zsh-autosuggestions"
    "$ZSH/plugins/git"
    "$ZSH/functions" "$ZSH/completions"
    "$ZSH_CUSTOM/functions" "$ZSH_CUSTOM/completions"
    "$ZSH_CACHE_DIR/completions" $fpath
  )
  print -rl -- '#omz revision: 157eb033f2a527021aac18c1f5850bbe3e3d223c' "#omz fpath: $term50_seed_fpath" >> "$ZSH_COMPDUMP"
  unset term50_seed_fpath
fi
source "$ZSH/oh-my-zsh.sh" || return
unset ZSH_AUTOSUGGEST_USE_ASYNC
# The original picker is deliberately loaded after OMZ key bindings.
source "$SANDBOX/data/shell/history.zsh"
unsetopt SHARE_HISTORY
setopt APPEND_HISTORY INC_APPEND_HISTORY HIST_IGNORE_DUPS HIST_IGNORE_SPACE
HISTSIZE=10000
SAVEHIST=5000
# A second input line gives long commands room on the Q20 display.
if [[ $ZSH_THEME == agnoster ]]; then
  PROMPT=$'%{%f%b%k%}$(build_prompt)\n%F{cyan}>%f '
  # Keep only the last directory components visible in a narrow terminal.
  prompt_dir() { prompt_segment "$AGNOSTER_DIR_BG" "$AGNOSTER_DIR_FG" '%2~'; }
  # Agnoster's general VCS collector starts many Git processes per prompt.
  # One porcelain status call supplies the same everyday branch/dirty view
  # without that process-launch cost on the Q20.
  prompt_git() {
    local output branch entry marks=''
    local -a entries
    local -i staged=0 unstaged=0
    output=$(command git status --porcelain=v1 --branch --untracked-files=normal --ignore-submodules=dirty 2>/dev/null) || return
    entries=("${(@f)output}")
    branch=${entries[1]#\#\# }
    branch=${branch#No commits yet on }
    branch=${branch#Initial commit on }
    branch=${branch%%...*}
    [[ $branch != 'HEAD (no branch)' ]] || branch='detached HEAD'
    for entry in "${entries[@]:1}"; do
      [[ $entry[1] == ' ' || $entry[1] == '?' ]] || staged=1
      [[ $entry[2] == ' ' ]] || unstaged=1
    done
    if (( staged || unstaged )); then
      prompt_segment "$AGNOSTER_GIT_DIRTY_BG" "$AGNOSTER_GIT_DIRTY_FG"
      (( staged )) && marks+=' ✚'
      (( unstaged )) && marks+=' ±'
    else
      prompt_segment "$AGNOSTER_GIT_CLEAN_BG" "$AGNOSTER_GIT_CLEAN_FG"
    fi
    print -rn -- " ${branch//\%/%%}$marks"
  }
fi
# Palette uses standard OSC controls already handled by the terminal core.
if [[ -t 1 ]]; then
  print -rn -- $'\e]10;#c0caf5\a\e]11;#1a1b26\a'
  local -a term50_palette=(1a1b26 f7768e 9ece6a e0af68 7aa2f7 bb9af7 7dcfff a9b1d6 565f89 f7768e 9ece6a e0af68 7aa2f7 bb9af7 7dcfff c0caf5)
  local -i term50_index
  for (( term50_index=1; term50_index<=16; term50_index++ )); do
    printf '\e]4;%d;#%s\a' $((term50_index-1)) "$term50_palette[term50_index]"
  done
  unset term50_palette term50_index
fi
