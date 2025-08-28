清理空格

```bash
find . -name "*.[chsS]" -exec sed 's/\s\+$//g' -i {} \;

cat 001.txt | sed '/^$/d' | sed -f abc.sed | pbcopy
```

```sed
s/）/) /g
s/（/ (/g
s/。/. /g
s/；/; /g
s/，/, /g
s/：/: /g
```
