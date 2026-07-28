import numpy as np
import tensorflow as tf

# 1. 명세서 기반 샘플 데이터 및 인텐트 정의
data = [
    # VOLUME_UP
    ("소리 키워줘", "VOLUME_UP"),
    ("볼륨 키워줘", "VOLUME_UP"),
    ("소리 키워", "VOLUME_UP"),
    ("볼륨 키워", "VOLUME_UP"),
    ("볼륨 올려", "VOLUME_UP"),
    ("잘 안 들려", "VOLUME_UP"),
    ("소리 좀 키워", "VOLUME_UP"),
    ("소리 확대해줘", "VOLUME_UP"),
    ("소리 업", "VOLUME_UP"),
    ("볼륨 업", "VOLUME_UP"),
    ("안들려", "VOLUME_UP"),
    # VOLUME_DOWN
    ("소리 줄여줘", "VOLUME_DOWN"),
    ("소리 줄여", "VOLUME_DOWN"),
    ("소리 작게", "VOLUME_DOWN"),
    ("볼륨 줄여줘", "VOLUME_DOWN"),
    ("볼륨 줄여", "VOLUME_DOWN"),
    ("시끄러", "VOLUME_DOWN"),
    ("볼륨 내려", "VOLUME_DOWN"),
    ("너무 시끄러워", "VOLUME_DOWN"),
    ("소리 작게 해줘", "VOLUME_DOWN"),
    ("줄여줘", "VOLUME_DOWN"),
    # MODE_TV
    ("TV 모드 해줘", "MODE_TV"),
    ("TV 모드", "MODE_TV"),
    ("TV", "MODE_TV"),
    ("티비 모드", "MODE_TV"),
    ("티비 볼래", "MODE_TV"),
    ("TV 모드로 변경", "MODE_TV"),
    # MODE_ONE_ON_ONE
    ("대화 모드 해줘", "MODE_ONE_ON_ONE"),
    ("일대일 모드로 바꿔", "MODE_ONE_ON_ONE"),
    ("사람 많네 대화 모드", "MODE_ONE_ON_ONE"),
    ("일대일", "MODE_ONE_ON_ONE"),
    ("대화", "MODE_ONE_ON_ONE"),
    # AUTO_MODE_OFF
    ("자동 모드 꺼줘", "AUTO_MODE_OFF"),
    ("자동 꺼", "AUTO_MODE_OFF"),
    ("수동", "AUTO_MODE_OFF"),
    ("자동 모드 꺼", "AUTO_MODE_OFF"),
    ("수동 모드", "AUTO_MODE_OFF"),
    ("수동 모드로 바꿔", "AUTO_MODE_OFF"),
    # UNKNOWN (예외 문장)
    ("오늘 날씨 어때", "UNKNOWN"),
    ("어...", "UNKNOWN"),
    ("오늘 며칠이야", "UNKNOWN"),
    ("며칠", "UNKNOWN"),
    ("날씨", "UNKNOWN"),
    (" ", "UNKNOWN")
]

# 2. 어휘 사전(vocab.txt) 및 라벨(labels.txt) 추출
vocab = set()
labels = sorted(list(set([label for _, label in data])))

for text, _ in data:
    for word in text.split():
        vocab.add(word)

vocab_list = sorted(list(vocab))
vocab_map = {word: i for i, word in enumerate(vocab_list)}
label_map = {label: i for i, label in enumerate(labels)}

# 파일 저장: vocab.txt & labels.txt
with open("vocab.txt", "w", encoding="utf-8") as f:
    for word in vocab_list:
        f.write(f"{word}\n")

with open("labels.txt", "w", encoding="utf-8") as f:
    for label in labels:
        f.write(f"{label}\n")

# 3. 데이터 전처리 (Max Sequence Length: 20)
MAX_LEN = 20


def preprocess(text):
    tokens = text.split()
    seq = [vocab_map.get(tok, 0) for tok in tokens]
    if len(seq) < MAX_LEN:
        seq += [0] * (MAX_LEN - len(seq))
    return seq[:MAX_LEN]


X = np.array([preprocess(text) for text, _ in data], dtype=np.float32)
y = np.array([label_map[label] for _, label in data], dtype=np.int32)

# 4. Keras 경량 신경망 모델 정의 및 학습
model = tf.keras.Sequential(
    [
        tf.keras.layers.Input(shape=(MAX_LEN,)),
        tf.keras.layers.Embedding(
            input_dim=len(vocab_list) + 1, output_dim=16
        ),
        tf.keras.layers.GlobalAveragePooling1D(),
        tf.keras.layers.Dense(16, activation="relu"),
        tf.keras.layers.Dense(len(labels), activation="softmax"),
    ]
)

model.compile(
    optimizer="adam",
    loss="sparse_categorical_crossentropy",
    metrics=["accuracy"],
)
model.fit(X, y, epochs=100, verbose=0)

# 5. TFLite 모델로 변환 및 저장 (intent_model.tflite)
converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()

with open("intent_model.tflite", "wb") as f:
    f.write(tflite_model)

print("✅ 성공적으로 3개 파일이 생성되었습니다!")
print("- intent_model.tflite")
print("- vocab.txt")
print("- labels.txt")