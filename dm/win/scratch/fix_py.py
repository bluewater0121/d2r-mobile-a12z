import io
p = r'C:\work\prj\d2r-mobile\dm\tools\patch_nso.py'
s = open(p, encoding='utf-8').read()
bad1 = "compat/tls_win.c)\\n    elif TARGET == 'android'"
good1 = "compat/tls_win.c)\n    elif TARGET == 'android'"
bad2 = "no red zone either\\n# 鸿蒙"
good2 = "no red zone either\n# 鸿蒙"
n1 = s.count(bad1); n2 = s.count(bad2)
s = s.replace(bad1, good1).replace(bad2, good2)
open(p, 'w', encoding='utf-8').write(s)
print('fixed', n1, n2)
