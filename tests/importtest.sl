; import test

(def b 100)
(def (expf) 18)

(assert !(bound? 'a))
(assert !(bound? 'c))
(import (exporttest))
(assert (bound? 'a))
(assert !(bound? 'c))
(assert (= 23 (get-c)))
(assert (= 18 (expf)))

(inc-c)
(inc-c)
(assert (= 25 (get-c)))

(output "import passed\n")

