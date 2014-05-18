AC_DEFUN([GP_CHECK_CYCLEAUTOCOMPLETE],
[
    GP_ARG_DISABLE([CycleAutocomplete], [auto])

    GP_CHECK_PLUGIN_DEPS([CycleAutocomplete], [CYCLEAUTOCOMPLETE], [$GP_GTK_PACKAGE >= 2.24])

    GP_COMMIT_PLUGIN_STATUS([CycleAutocomplete])

    AC_CONFIG_FILES([
        cycleautocomplete/Makefile
        cycleautocomplete/src/Makefile
    ])
])
