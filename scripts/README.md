# Visualization manual

## Simple instruction
```
$ ./report.py --data <daptrace result data> --name <experiment name>
```
For example,
```
$ ./report.py --data example_data/example.data --name example
```
Using the command, you can find result file `<name>-<pid>.pdf` generated in
the current directory.

## Deeper use

### Increase or decrease y-axis resolution
Use `--column` option.
```
$ ./report.py --column 400 [...]
```

### If the access pattern is not dark enough
Use `--vmax` option to decrease the color scale; you can see the default value
in the colorbar without specifying the option.
```
$ ./report.py --vmax 10 [...]
```

### Show address in different formats
- Hexadecimal
```
$ ./report.py --hex [...]
```
- Plain (no tera, giga, mega, etc.)
```
$ ./report.py --plain [...]
```
- The number of decimal places
```
$ ./report.py --digits 7 [...]
```

### Interactive mode
Use `--interactive` option.
```
$ ./report.py --interactive [...]
```
