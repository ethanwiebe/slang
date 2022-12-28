(pair (pair 1 2) 2)

(def first (pair (pair 1 2) 3))
(setL! (L first) first)

(def a (pair 1 2))

(def sec (pair a a))

(print sec)
(setL! sec 5)
(print sec)
