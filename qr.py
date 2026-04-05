import cv2
import numpy as np
import math

# QR Detector
class QRDetector:
    def __init__(self):
        self.detector = cv2.QRCodeDetector()

    def detect(self, frame):
        # detectAndDecodeMulti ищет сразу все QR на изображении
        retval, decoded_info, points, _ = self.detector.detectAndDecodeMulti(frame)
        objects = []

        if retval:
            for pts in points:
                pts = pts.astype(int)
                x, y, w, h = cv2.boundingRect(pts)
                contour = pts.reshape(-1, 1, 2)
                objects.append((contour, (x, y, w, h), pts))
        return objects

# Analyzer
class ObjectAnalyzer:
    def __init__(self, w, h):
        self.cx = w // 2
        self.cy = h // 2

    def get_center(self, contour):
        M = cv2.moments(contour)
        if M["m00"] == 0:
            return None
        return (int(M["m10"]/M["m00"]), int(M["m01"]/M["m00"]))

    # угол до центра кадра
    def get_angle_to_center(self, point):
        dx = point[0] - self.cx
        dy = self.cy - point[1]
        return math.degrees(math.atan2(dy, dx))

    # расстояние до центра кадра
    def get_distance(self, point):
        dx = point[0] - self.cx
        dy = point[1] - self.cy
        return math.sqrt(dx*dx + dy*dy)

    # реальный угол наклона QR по верхней стороне
    def get_qr_orientation(self, pts):
        # pts: 4 точки QR в порядке (tl, tr, br, bl)
        # используем верхнюю линию tl->tr
        dx = pts[1][0] - pts[0][0]
        dy = pts[1][1] - pts[0][1]
        angle = math.degrees(math.atan2(dy, dx))
        return angle

# MAIN
def main():
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("Не удалось открыть камеру")
        return

    detector = QRDetector()

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        # предобработка для улучшения детекции QR
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        gray = cv2.equalizeHist(gray)
        proc_frame = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)

        objects = detector.detect(proc_frame)

        h, w = frame.shape[:2]
        analyzer = ObjectAnalyzer(w, h)

        # центр кадра
        center = (w // 2, h // 2)
        cv2.circle(frame, center, 6, (0, 0, 255), -1)

        for contour, (x, y, w_box, h_box), pts in objects:
            # прямоугольник вокруг QR
            cv2.rectangle(frame, (x, y), (x + w_box, y + h_box), (255, 0, 0), 3)

            # центр QR
            obj_center = analyzer.get_center(contour)
            if obj_center is None:
                continue
            cv2.circle(frame, obj_center, 6, (255, 0, 0), -1)

            # линия до центра
            cv2.line(frame, center, obj_center, (0, 0, 255), 2)

            # угол и расстояние
            angle_to_center = analyzer.get_angle_to_center(obj_center)
            distance = analyzer.get_distance(obj_center)
            qr_angle = analyzer.get_qr_orientation(pts)

            print(f"QR Orientation: {qr_angle:.1f}°, Angle to center: {angle_to_center:.1f}°, Distance: {distance:.1f}px")

            # текст на изображении
            cv2.putText(frame, f"O:{int(qr_angle)}°",
                        (x, y - 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2)
            cv2.putText(frame, f"A:{int(angle_to_center)}°",
                        (x, y - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2)
            cv2.putText(frame, f"D:{int(distance)}px",
                        (x, y + h_box + 20), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)

        # масштабируем окно, если кадр слишком большой
        max_width = 1000
        if frame.shape[1] > max_width:
            scale = max_width / frame.shape[1]
            frame = cv2.resize(frame, None, fx=scale, fy=scale)

        cv2.namedWindow("QR Tracking", cv2.WINDOW_NORMAL)
        cv2.imshow("QR Tracking", frame)

        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    cap.release()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()