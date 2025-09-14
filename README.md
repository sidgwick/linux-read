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

# compile bochs from source

```console
brew install --build-from-source --formula bochs.rb
```

## TODO

1. 下面这个函数, 是怎么返回 tm 结构的? 这里面不涉及到栈上内存释放/堆上内存泄漏吗?

```c
struct tm *gmtime(const time_t *tp);
```
