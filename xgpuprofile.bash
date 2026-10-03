# bash completion for xgpuprofile

_xgpuprofile()
{
	local cur prev w acted=0
	local actions="--status --mode --once --refresh --restart-x"
	local opts="--config --dry-run --verbose --version --help"

	cur=${COMP_WORDS[COMP_CWORD]}
	prev=${COMP_WORDS[COMP_CWORD-1]}

	# --opt=value splits into three words because '=' is a wordbreak.
	if [[ $cur == = ]]; then
		cur=
	elif [[ $prev == = ]]; then
		prev=${COMP_WORDS[COMP_CWORD-2]}
	fi

	case $prev in
	--mode)
		COMPREPLY=($(compgen -W "hybrid auto dgpu" -- "$cur"))
		return ;;
	--once)
		COMPREPLY=($(compgen -W "hybrid dgpu" -- "$cur"))
		return ;;
	--config)
		COMPREPLY=($(compgen -f -- "$cur"))
		compopt -o filenames 2>/dev/null
		return ;;
	esac

	for w in "${COMP_WORDS[@]:1:COMP_CWORD-1}"; do
		case " $actions " in *" $w "*) acted=1 ;; esac
	done

	if ((acted)); then
		COMPREPLY=($(compgen -W "$opts" -- "$cur"))
	else
		COMPREPLY=($(compgen -W "$actions $opts" -- "$cur"))
	fi
}

complete -F _xgpuprofile xgpuprofile
