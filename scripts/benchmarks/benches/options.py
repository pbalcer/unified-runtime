from dataclasses import dataclass

@dataclass
class Options:
    sycl: str = ""
    rebuild: bool = True

options = Options()

