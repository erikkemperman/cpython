# Test ``inspect.formatannotation``
# https://github.com/python/cpython/issues/96073

from typing import Intersection, Union, List

ann = Intersection[List[str], int]

# mock typing._type_repr behaviour
class A: ...

A.__module__ = 'testModule.typing'
A.__qualname__ = 'A'

ann1 = Intersection[List[A], int]

ann2 = Union[List[str], int]

# mock typing._type_repr behaviour
class B: ...

B.__module__ = 'testModule.typing'
B.__qualname__ = 'B'

ann3 = Union[List[A], int]
