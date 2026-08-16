# OROS — Production template

## Differences
The production template is an autonomous ready to build image. All services and drivers runs continuously.
If someone want to modify the loops of each thread, entry points/hooks are already available and need to be complete with code.
This is the folder you use when ready to operate a production system.

### Programmer API / Entry point (`app/`)

```
app/app.h          complete contract (hooks + real-time rules)
app/app_core0.c    TODO: EtherCAT PDO logic (16-bit DI -> 16-bit DO)
app/app_core1.c    TODO: image processing + PLC logic (app_pi[] / app_po[])
app/app_core2.c    (infrastructure) + optional TODO: app_on_key()
app/app_core3.c    TODO: lightweight periodic processing
```
