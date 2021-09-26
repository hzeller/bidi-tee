Bi-directional tee. Essentially observing bi-directional stdin/stdout/stderr
chatter and writing all of them into a file, color coded.

```
 bidi-tee /tmp/output.log -- other-program parameters to other program
```

The output in `/tmp/output.log` will contain the whole sequence in a single
file but terminal color coded.

 * parent:stdin->child: red bold
 * child:stdout->parent: blue bold
 * child:stderr->parent: regular

Useful to debug communication with a subprocess, here an example snippet
from a clangd session, observing what is going on between emacs and clangd
talking LSP (red: emacs talking to clangd, blue: clangd answering, green: clangd stderr output).

![](./img/bidi-tee.png)
