# Git shortcuts. Load into the current shell with:
#   source git-shortcuts.bash
# (or add that line, with the full path, to ~/.bashrc)
#
# Names avoid gs (ghostscript) and gc (graphviz).

alias g='git'
alias gs='git status -sb'
alias ga='git add'
alias gaa='git add -A'
alias gcm='git commit -m'
alias gd='git diff'
alias gds='git diff --staged'
alias glg='git log --oneline --graph --decorate -20'
alias gb='git branch'
alias gsw='git switch'
alias gswc='git switch -c'
alias gpl='git pull'
alias gp='git push'

# Tab completion for `g`, when bash's git completion is loaded.
if declare -F __git_complete >/dev/null; then
  __git_complete g __git_main
fi
