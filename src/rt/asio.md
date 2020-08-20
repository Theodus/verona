
## Verona Async I/O

### Timers
```verona
let timer = Timer(100ms);
let notify: Promise[Timer] = io::subscribe();
verona::rt::io::subscribe(timer);
let printer = Cown[Printer]();
when (printer, notify) { printer.print("timeout!"); }
```

### Readv
```verona
  let file = File.open("foo.txt");
  let io_vecs = mk_file_read_bufs(BLOCK_SIZE, (file.len() / BLOCK_SIZE) + 1);
  let readv = Readv(file, io_vecs);
  let notify: Noticeboard[Readv] = verona::rt::io::subscribe(readv);
  let printer = Cown[Printer]();
  when (printer, notify) { printer.printv(notify.bufs); }
```

### Question

What interface to noticeboards do we want to expose through the Verona language? Would it be included in a `when` like the following example, and have the `notified()` function dispatch on the noticeboard identity? Or would it be something else?
```
foo(notify: Noticeboard[Timer], printer: Cown[Printer])
{
  when (printer, notify) { printer.print("timeout!"); }
}
```
