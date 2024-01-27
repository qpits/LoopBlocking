# Passo di trasformazione Loop Blocking
Il passo punta ad implementare Loop Blocking (o Tiling) su una struttura di loop nidificata semplice.

Implementazione consiste in:
- individuazione loop candidati
- trovare tra candidati quelli che possono essere trasformati in sicurezza
- trasformazione codice.

Loop dovranno essere in Simplify Form.

## Struttura loop
Cerco un loop nest a due livelli:
```
for i in LB1,UB2:
    for j in LB2,UB2:
        body
```

Il loop interno è il target della trasformazione. Mi limito per semplicità a individuare questa struttura solo per loop a depth 1 e 2.
Il loop esterno non deve contenere altri loop oltre al target.
Tra i due header dei loop non devono esserci istruzioni: header del loop esterno punta a preheader del loop interno, il quale contiene solamente istruzione di salto verso header; questa condizione è fondamentale per applicazione della trasformazione.
### Q
- Posso considerare loop a depth 2 che hanno altri loop al loro interno?

## Algo

Per prima cosa determino quali potrebbero essere i loop candidati per il blocking.
I candidati sono quelli a livello 2 nel nest, che non contengono altri loop e che sono gli unici figli del loop genitore.
**Idea**: Posso raccogliere candidati in structs in cui salvo tutti gli "elementi" che serviranno preventivamente, in modo da averli già cachati senza dover accedere sempre alla classe Loop.

I candidati devono soddisfare alcune condizioni prima che possa essere effettuata la trasformazione:
- il preheader deve essere vuoto: singola istruzione di branch verso header;
- singolo exit block;
- loop genitore deve avere singolo exit block;
- i value che determinano i boundary del loop devono dominare l'header del loop genitore oltre che quello del loop stesso.

**Ipotesi**: non deve esserci dipendenza tra i bounds dei due loop. non posso avere loop interno che fa riferimento a bounds del loop esterno

L'ultima condizione è necessaria per poter creare il loop esterno che effettua il blocking.
Il nuovo loop creato dalla trasformazione avrà come bounds gli stessi bounds del loop interno: se i Value che li raprresentano non dominano l'header del loop genitore, un loro uso nell'header del nuovo loop (che dominerà l'header del loop genitore) violerebbe la forma SSA.

Alcuni esempi:

![esempio 1](../test/cfg_value_nodom_ex1.svg)

In questo esempio, il Value che fornisce il boundary del loop è ```%7```, il quale non è definito prima dell'header del loop "rosso"; non posso utilizzarlo prima della sua definizione, quindi sicuramente non prima dell'header del loop "giallo"

![esempio 2](../test/cfg_value_nodom_ex2.svg)

Qui il Value di interesse è ```%5```, che è definito nel preheader del loop "rosso" e non può essere utilizzato nel ph di quello giallo.

**Nota 1**: nel secondo esempio l'istruzione ```%5``` utilizza unicamente il Value ```%0```, che domina l'header del loop più esterno -> in questo caso sarebbe possibile effettuare _hoisting_ dell'istruzione e portarla nel ph del loop esterno. Date queste considerazioni, la condizione di preheader vuoto potrebbe essere rilassata; per semplicità si tralscerà questa casistica.

**Nota 2**: nel caso in cui questi Value siano costanti, non si pone il problema della dominanza, basta semplicemente effettuare una copia

Con il candidato deve essere anche determinato un loop "target", che diventerà figlio del loop creato dalla trasformazione. Per iniziare il target considerato sarà il loop genitore del candidato

I candidati vengono raggruppati in una struttura dati, la quale viene data in input al metodo che esegue la trasformazione.

L'informazione sulla dimensione del blocco viene letta da una variabile di ambiente.

### Trasformazione

Trasformazione consiste in:
- creare header loop esterno -> preheader del loop genitore diventa header del nuovo loop, quindi occorre creare un nuovo preheader per il loop genitore in modo da preservare la forma simplified.
- creare latch loop esterno -> exit block loop genitore punta a questo latch
- creare exit block loop esterno -> si va a sostituire all'exit block del loop genitore
- creare nuova struttura Loop per LLVM
- aggiornare analisi (LoopInfo, DominatorTree)

Seguono dettagli tecnici sulla trasformazione per riferimento.

Per prima cosa, richiedo a LoopInfo di allocare un nuovo Loop -> ```Loop* NewLoop = LI->AllocateLoop()```
Ora devo creare i nuovi blocchi: Header, preheader per il Parent, Latch.

La struttura sarà la seguente:
ParentPreheader -> NewHeader -> NewParentPreheader -> ParentHeader

In NewHeader saranno utilizzati i boundary del loop interno:
```
for (i = LB_target; i < UB_target; i += BlockingFactor)
```

LB_target si trova nella phi instruction che rappresenta l'induction variable del loop; UB_target si trova nella cmp instruction che fornisce la condizione del loop.
**Nota**: sembra che ciò sia sempre valido, soprattutto con i constraint specificati precedentemente (come adiacenza tra gli header). Vedendo un esempio:
```c
#include <stdio.h>
void test(int A) {
    for (int i = 0; i < A && i < 10; i++) {
        for (int j = i; j < 100 && j < A; j++) {
            printf("test");
        }
    }
}
```

si traduce in:

![test con condizioni loop complesse](../test/test_complex_exit_cond.svg)

anche in questo caso, i Value che stiamo cercando sono tutti nell'header.

**Nota 2**: la condizione su singolo exit block sembra essere abbastanza restrittiva; si veda il seguente esempio (a cui è stato applicato il paso "loop-simplify"):

```c
#include <stdio.h>
void test(int A) {
    for (int i = 0; i < 100; i++) {
        for (int j = i; j < 100; j++) {
            if (j < A)
                break;
            printf("test");
        }
    }
}
```
![test su effetti strutture di controllo](../test/cfg_multiple_exit.svg)

Il loop interno potrebbe essere tranquillamente valido, tuttavia non lo è allo stato attuale proprio per il constraint sul singolo exit block.

## Update analisi

In totale sono stati aggiunti 2 BB (nuovo header e nuovo latch) per il loop di blocking.

**IMPORTANTE**: Questi due nuovi blocchi devono essere aggiunti alla classe Loop attraverso il metodo ```addBlockEntry``` (vedi https://llvm.org/doxygen/classllvm_1_1LoopBase.html#ab0a5e875687fec396caa916b3950e0a3)

Questi due BB devono essere aggiunti al DominatorTree: il nuovo header è dominato dal preheader del loop parent, il nuovo latch è (per definizione di loop) dominato dal nuovo header.

Passando agli archi del domtree, si ha:
- eliminazione arco preheader-parentheader;
- nuovo arco tra preheader-newheader;
- nuovo arco tra newheader-parentheader;
- nuovo arco tra newheader-newlatch;

Due strade per update a domtree:
- vettore di DominatorTree::UpdateType;
- chiamata diretta a primitive (DominatorTree::addNewBlock(), DominatorTree::insertEdge(), DominatorTree::deleteEdge()).

Il primo modo permette di effettuare gli update in modalità lazy: vengono applicati nel modo migliiore in base ad un algoritmo; non devo preoccuparmi dell'ordine.
Utilizzando le primitive invece, occorre stare attenti all'ordine: la documentazione di questi metodi dice esplicitamente **"This function has to be called just before or just after making the update on the actual CFG. There cannot be any other updates that the dominator tree doesn't know about."**.

La precedente osservazione porta a dover porre attenzione all'ordine di chiamata quando si effettuano gli update: se ad esempio si vuole aggiungere un nodo al dominator tree in modo che questo ne vada a sostituire un altro, (apparentemente) occore **prima aggiungere** il nuovo edge (o aggiungere direttamente il nuovo nodo), poi eliminare quello vecchio; se si effettua l'eliminazione del vecchio edge prima di aggiungere quello nuovo il domtree viene aggiornato SENZA TENERE CONTO che potrebbe nascere un nuovo edge, perciò se il subtree raggiunto da quell'edge diventa irraggiungibile, l'algoritmo di update procederà ad eliminarlo!

Per update LoopInfo, occorre prima di tutto **rimuovere** il loop parent e aggiungerlo come figlio del nuovo loop creato. In seguito, occorre aggiungere il nuovo loop ai top-level loops; infine tutti i basic blocks che apparterranno al nuovo loop (in questo caso tutti quelli del loop figlio) devono essere aggiunti.


# Cost analysis
refs:
- https://reviews.llvm.org/D63459
- https://llvm.org/doxygen/LoopCacheAnalysis_8cpp_source.html
- https://llvm.org/doxygen/LoopCacheAnalysis_8h_source.html

Utilizzare IndexedReference per ottenere analisi su memory reference in un loop -> https://llvm.org/doxygen/classllvm_1_1IndexedReference.html
Utilizzare CacheCost per calcolare costo accessi di un inner loop -> https://llvm.org/doxygen/classllvm_1_1CacheCost.html

# Reuse analysis
 refs:
- https://llvm.org/doxygen/classllvm_1_1LoopNest.html

decido quale tipo di strutture voglio considerare -> loop perfettamente innestati di profondità 2 e 3, con root del nest a qualsiasi livello.
