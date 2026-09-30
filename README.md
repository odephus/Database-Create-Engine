# DBCE database create engine

## compile
gcc -Wall -Wextra -O2 main.c dbce.c -o dbce.exe -lpthread

## CLI
.\dbce.exe create db users  
.\dbce.exe db list  
.\dbce.exe add users id:1 '{"name":"Alper"}'  
.\dbce.exe get users id:1  
.\dbce.exe get users table  
.\dbce.exe del users id:1  
.\dbce.exe go users  

## REPL
add id:1 {"name":"Alper"}
get id:1
get table
del id:1
cls
exit

## Config (config.json)
{
  "db_path": "data.dbce",
  "queue_size": 2048,
  "sync_interval_ms": 100,
  "lang": "tr"
}