# builds data/www/disclaimer.html from DISCLAIMER.txt: python3 gen_disclaimer.py DISCLAIMER.txt <www dir>
import html, re, sys
src = open(sys.argv[1], newline='').read().replace('\r\n', '\n'); www = sys.argv[2]
page = open(www + '/disclaimer.html').read()
lines = src.split('\n'); out = []; para = []; lst = None; cur = None
def flush():
    global para
    if para: out.append('<p>' + html.escape(' '.join(para)) + '</p>'); para = []
def end_item():
    global cur
    if cur is not None: out.append('<li>' + html.escape(cur) + '</li>'); cur = None
def close_list():
    global lst
    end_item()
    if lst: out.append('</%s>' % lst); lst = None
for l in lines[1:]:
    s = l.rstrip()
    if not s.strip(): flush(); close_list(); continue
    m = re.match(r'^\s*(\*|\d+\.)\s+(.*)$', s)
    if m:
        flush(); end_item(); kind = 'ol' if m.group(1)[0].isdigit() else 'ul'
        if lst != kind: close_list(); out.append('<%s>' % kind); lst = kind
        cur = m.group(2); continue
    if cur is not None and s.startswith('   '): cur += ' ' + s.strip(); continue
    if s.isupper() and len(s) < 40: flush(); close_list(); out.append('<h5>' + html.escape(s) + '</h5>'); continue
    if s.startswith('BY USING'): flush(); out.append('<p class="font-weight-bold text-danger">' + html.escape(s)); continue
    if out and out[-1].startswith('<p class="font-weight-bold') and not out[-1].endswith('</p>'): out[-1] += ' ' + html.escape(s) + '</p>'; continue
    para.append(s.strip())
flush(); close_list()
start = page.index('<h4 class="card-title text-danger">'); start = page.index('\n', start) + 1
end = page.index('        <div class="mt-3"><button')
page = page[:start] + '        ' + '\n        '.join(out) + '\n' + page[end:]
open(www + '/disclaimer.html', 'w').write(page)
print('ok', len(out), 'blocks')
