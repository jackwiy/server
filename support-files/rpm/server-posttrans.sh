if [ -r %{restart_flag} ] ; then
  # only restart the server if it was already running
  if [ -x /usr/bin/systemctl ] ; then
    /usr/bin/systemctl daemon-reload
    /usr/bin/systemctl -q is-active mariadb.service && \
      START='/usr/bin/systemctl start mariadb.service'
    /usr/bin/systemctl stop mariadb.service
  elif %{_sysconfdir}/init.d/mysql status > /dev/null 2>&1; then
    START='%{_sysconfdir}/init.d/mysql start'
    %{_sysconfdir}/init.d/mysql stop
  fi
  for p in `cat %{restart_flag}`; do
    # if %{restart_flag} includes a plugin name it can only mean
    # that a new plugin was installed. At least for now.
    if test -f %{_mysharedir}/$p-postin.sql; then
      %{_bindir}/mysql_install_db --rpm --user=%{mysqld_user} --sql-script=%{_mysharedir}/$p-postin.sql
    fi
  done
  rm %{restart_flag}
  $START
fi
