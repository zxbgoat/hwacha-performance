"""日志/踪迹文件的打开：透明支持 gzip（路径以 .gz 结尾，或给出的路径不存在但同名 .gz 存在）。"""
import gzip, os

def openlog(path):
    if path.endswith('.gz'):
        return gzip.open(path, 'rt', errors='replace')
    if not os.path.exists(path) and os.path.exists(path + '.gz'):
        return gzip.open(path + '.gz', 'rt', errors='replace')
    return open(path, errors='replace')

def resolve(path):
    """返回实际存在的路径（可能带 .gz），给 C++ 模型等外部程序用。"""
    if not os.path.exists(path) and os.path.exists(path + '.gz'):
        return path + '.gz'
    return path
