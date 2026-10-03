#compdef xgpuprofile

_arguments -S \
	'(- *)--help[print help and exit]' \
	'(- *)--version[print version and exit]' \
	'(--mode --once --refresh --restart-x)--status[show mode, detection and sync state]' \
	'(--status --once --refresh --restart-x)--mode=[set and save the mode]:mode:(hybrid auto dgpu)' \
	'(--status --mode --refresh --restart-x)--once=[stage a mode for this session only]:mode:(hybrid dgpu)' \
	'(--status --mode --once --restart-x)--refresh[re-decide from the saved config]' \
	'(--status --mode --once --refresh)--restart-x[restart the display manager]' \
	'--config=[use a different configuration file]:file:_files' \
	'--dry-run[with --refresh, change nothing]' \
	'--verbose[explain each step on stderr]'
