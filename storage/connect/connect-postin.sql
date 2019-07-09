create view mysql.connect_status as
  select support as status from information_schema.engines
  where engine='connect';
