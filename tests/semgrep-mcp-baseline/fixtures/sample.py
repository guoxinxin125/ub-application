def unsafe_input(user_input):
    return eval(user_input)


def safe_length(value):
    return len(value)


def unsafe_literal():
    return eval("1 + 2")
