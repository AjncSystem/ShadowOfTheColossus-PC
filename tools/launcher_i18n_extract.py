import ast, json, sys
src=open(sys.argv[1],encoding='utf-8').read()
tree=ast.parse(src)
pairs={}
def lit(n):
    return n.value if isinstance(n,ast.Constant) and isinstance(n.value,str) else None
for node in ast.walk(tree):
    if isinstance(node,ast.Call) and isinstance(node.func,ast.Name) and node.func.id=='_' and len(node.args)==2:
        pt,en=lit(node.args[0]),lit(node.args[1])
        if pt is not None and en is not None: pairs[pt]=en
    # tuples (pt, en) used in option lists
    if isinstance(node,ast.Tuple) and len(node.elts)==2:
        pt,en=lit(node.elts[0]),lit(node.elts[1])
        if pt is not None and en is not None and pt!=en and any(c in pt for c in ' ãçéáíóõú') : pairs.setdefault(pt,en)
json.dump(pairs,open(sys.argv[2],'w',encoding='utf-8'),ensure_ascii=False,indent=0)
print(len(pairs), sum(len(v) for v in pairs.values()))
