Bi-directional tee. Essentially observing bi-directional stdin/stdout/stderr
chatter and writing all of them into a file, color coded.

```
 bidi-tee /tmp/output.log -- other-program parameters to other program
```

The output in `/tmp/output.log` will contain the whole sequence in a single
file but terminal color coded.

 * parent:stdin->child: red bold
 * child:stdout->parent: blue inverse
 * child:stderr->parent: blue

Color can't be shown with github markdown, so this looks bo
<pre>
<b>Request from parent to child</b>
Response coming back from the child
different color stderr from child
<b>Another parent child communication</b>
.. and its response
</pre>

Useful to debug communication with a subprocess.
