handle SIGUSR1 nostop
handle SIGUSR2 nostop

# Quit on normal completion, but not on signals
define hookpost-run
  if $_exitcode != -1
    quit
  end
end
